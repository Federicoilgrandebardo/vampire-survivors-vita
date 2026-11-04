// Audio worker. See audio_worker.h.
//
// Why: raylib decodes the music in UpdateMusicStream while it holds its global audio lock. The mixer thread holds
// the same lock while it mixes all the sounds. On the Vita the main thread lost 5.4 ms per frame there (benchmark
// 2026-11-04: 49 fps with sound, 73 fps without). The first use of a sound effect also loaded it on the main
// thread: 70 ms for one sound.
//
// This file is outside the unity build of the game (vs_game.c), because the game code replaces malloc with gmem,
// and gmem is not thread-safe. Only raylib calls run on the threads here.

#include <string.h>

#include "audio_worker.h"

#ifdef __vita__
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#define AW_MUSIC_MAX 8
#define AW_REQ_MAX 16

static int started;
static SceUID music_mutex;
// ending: 0 playing, 1 the last decoded frames are queued, 2 a silent chunk is queued after them (see music_fill)
static struct { Music *m; int ending; } tracked[AW_MUSIC_MAX];

// The stb_vorbis decoder inside libraylib.a (exported). raylib opens it in LoadMusicStream (Music.ctxData).
typedef struct stb_vorbis stb_vorbis;
int stb_vorbis_get_samples_short_interleaved(stb_vorbis *f, int channels, short *buffer, int num_shorts);
int stb_vorbis_seek_start(stb_vorbis *f);
#define MUSIC_AUDIO_OGG 2  // raudio.c: NONE, WAV, OGG, ...
// Frames in one half of a stream buffer: SetAudioStreamBufferSizeDefault in aw_start makes every music stream use
// it. 4096 frames = 93 ms at 44.1 kHz, thus a buffer holds 186 ms.
#define AW_CHUNK 4096
static short pcm[AW_CHUNK * 2];

// UpdateMusicStream of raylib decodes while it holds the global audio lock, and every PlaySound of the main thread
// waits for it (benchmark: up to 18 ms). Here the decoding has no lock. UpdateAudioStream takes the lock only for the
// copy. The main thread does not touch the decoder at the same time: Stop and Seek take music_mutex
// (aw_music_stop, aw_music_seek), and a stream leaves the list before its unload.
static void music_fill(int t) {
    Music mu = *tracked[t].m;
    if (mu.ctxType != MUSIC_AUDIO_OGG || mu.stream.sampleSize != 16 || mu.stream.channels > 2) {
        UpdateMusicStream(mu);  // WAV and others: the raylib path
        return;
    }
    while (IsAudioStreamProcessed(mu.stream)) {  // one half of the buffer is free
        if (tracked[t].ending == 2) {  // the last frames played: the end of the track
            StopMusicStream(mu);
            tracked[t].ending = 0;
            return;
        }
        if (tracked[t].ending == 1) {  // the last frames play now: a silent chunk after them
            UpdateAudioStream(mu.stream, pcm, 0);
            tracked[t].ending = 2;
            continue;
        }
        int ch = mu.stream.channels, got = 0, seeks = 0;
        while (got < AW_CHUNK) {
            int n = stb_vorbis_get_samples_short_interleaved((stb_vorbis *)mu.ctxData, ch, pcm + got * ch, (AW_CHUNK - got) * ch);
            if (n > 0) { got += n, seeks = 0; continue; }
            if (!tracked[t].m->looping || ++seeks > 1) break;  // no loop, or a file without samples
            stb_vorbis_seek_start((stb_vorbis *)mu.ctxData);  // loop: from the start
        }
        UpdateAudioStream(mu.stream, pcm, got);  // fewer frames: raylib fills the rest with zeros
        if (got < AW_CHUNK) tracked[t].ending = 1;
    }
}

// One slot for each queued load. state: 0 free, 1 queued, 2 loaded (the main thread takes it and frees the slot).
typedef struct {
    volatile int state;
    int id;
    char path[256];
    Sound s;
    double ms;
} Req;
static Req req[AW_REQ_MAX];
static SceUID loader_sema;  // signaled for each new request

// Sound effect commands. The raylib mixer holds the global audio lock while it mixes all the sounds, and
// PlaySound, SetSoundVolume, SetSoundPitch and StopSound wait for that lock (benchmark: p95 7 ms in physics and timer
// callbacks). The main thread only writes a command here. The sound thread runs it and waits for the lock.
// One writer (the main thread) and one reader (the sound thread): a ring without a lock.
// The stream of a Sound and of a Music is an AudioStream: one command type for both.
enum { CMD_PLAY, CMD_STOP, CMD_VOLUME };
typedef struct { int op; AudioStream st; float volume, pitch; } SfxCmd;
#define AW_SFX_Q 256
static SfxCmd sfx_q[AW_SFX_Q];
static volatile unsigned sfx_head, sfx_tail;  // head: written by the main thread; tail: by the sound thread
static SceUID sfx_sema;

static int sfx_thread(SceSize args, void *argp) {
    (void)args, (void)argp;
    for (;;) {
        sceKernelWaitSema(sfx_sema, 1, NULL);
        while (sfx_tail != sfx_head) {
            __sync_synchronize();  // the command is complete before it is read
            SfxCmd c = sfx_q[sfx_tail % AW_SFX_Q];
            if (c.op == CMD_STOP) StopAudioStream(c.st);
            else if (c.op == CMD_VOLUME) SetAudioStreamVolume(c.st, c.volume);
            else SetAudioStreamVolume(c.st, c.volume), SetAudioStreamPitch(c.st, c.pitch), PlayAudioStream(c.st);
            __sync_synchronize();
            sfx_tail++;
        }
    }
    return 0;
}

static int sfx_push(int op, AudioStream st, float volume, float pitch) {
    if (sfx_head - sfx_tail >= AW_SFX_Q) return 1;  // full: this command is skipped (the game does not wait)
    sfx_q[sfx_head % AW_SFX_Q] = (SfxCmd){ op, st, volume, pitch };
    __sync_synchronize();  // the command is complete before the sound thread can see it
    sfx_head++;
    sceKernelSignalSema(sfx_sema, 1);  // fails only when the count is at its maximum: the thread is awake then
    return 1;
}
int aw_sfx_play(Sound s, float volume, float pitch) { return started ? sfx_push(CMD_PLAY, s.stream, volume, pitch) : 0; }
int aw_sfx_stop(Sound s) { return started ? sfx_push(CMD_STOP, s.stream, 0, 0) : 0; }
int aw_music_volume(Music *m, float volume) { return started ? sfx_push(CMD_VOLUME, m->stream, volume, 0) : 0; }
// A queued command can name a stream that is about to unload: the queue must be empty first
static void sfx_drain(void) {
    while (sfx_tail != sfx_head) sceKernelDelayThread(200);
}

static int music_thread(SceSize args, void *argp) {
    (void)args, (void)argp;
    for (;;) {
        sceKernelLockMutex(music_mutex, 1, NULL);
        for (int i = 0; i < AW_MUSIC_MAX; i++)
            if (tracked[i].m) music_fill(i);
        sceKernelUnlockMutex(music_mutex, 1);
        sceKernelDelayThread(5000);  // a buffer holds 186 ms
    }
    return 0;
}

static int loader_thread(SceSize args, void *argp) {
    (void)args, (void)argp;
    for (;;) {
        sceKernelWaitSema(loader_sema, 1, NULL);
        for (int k = 0; k < AW_REQ_MAX; k++) {
            if (req[k].state != 1) continue;
            SceInt64 t = sceKernelGetProcessTimeWide();
            req[k].s = LoadSound(req[k].path);
            req[k].ms = (sceKernelGetProcessTimeWide() - t) / 1000.0;
            __sync_synchronize();  // the sound is complete before the main thread can see state 2
            req[k].state = 2;
        }
    }
    return 0;
}

void aw_start(void) {
    if (started || !IsAudioDeviceReady()) return;
    music_mutex = sceKernelCreateMutex("vs_music", 0, 0, NULL);
    loader_sema = sceKernelCreateSema("vs_loader", 0, 0, AW_REQ_MAX, NULL);
    sfx_sema = sceKernelCreateSema("vs_sfx", 0, 0, AW_SFX_Q, NULL);
    if (music_mutex < 0 || loader_sema < 0 || sfx_sema < 0) return;
    SetAudioStreamBufferSizeDefault(AW_CHUNK);  // before the first LoadMusicStream: music_fill writes whole halves
    // The music thread has a higher priority than the main thread (a lower number), so the music never runs out.
    // The loader thread has a lower priority. Each one has its own core, not core 0 (the core of the main thread
    // in most homebrew).
    SceUID m = sceKernelCreateThread("vs_music", music_thread, 0x10000100 - 10, 256 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    SceUID l = sceKernelCreateThread("vs_loader", loader_thread, 0x10000100 + 10, 256 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
    // The sound thread has the highest priority of the three: a command waits only for the mixer
    SceUID x = sceKernelCreateThread("vs_sfx", sfx_thread, 0x10000100 - 20, 64 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (m < 0 || l < 0 || x < 0) return;
    sceKernelStartThread(m, 0, NULL);
    sceKernelStartThread(l, 0, NULL);
    sceKernelStartThread(x, 0, NULL);
    started = 1;
}

int aw_threaded(void) { return started; }

void aw_music_track(Music *m) {
    if (!started) return;
    sceKernelLockMutex(music_mutex, 1, NULL);
    int free_slot = -1, have = 0;
    for (int i = 0; i < AW_MUSIC_MAX; i++) {
        if (tracked[i].m == m) have = 1, tracked[i].ending = 0;  // played again after a stop
        if (!tracked[i].m && free_slot < 0) free_slot = i;
    }
    if (!have && free_slot >= 0) tracked[free_slot].m = m, tracked[free_slot].ending = 0;
    sceKernelUnlockMutex(music_mutex, 1);
}

void aw_music_untrack(Music *m) {
    if (!started) return;
    sfx_drain();
    sceKernelLockMutex(music_mutex, 1, NULL);  // waits for the end of an update of the music thread
    for (int i = 0; i < AW_MUSIC_MAX; i++)
        if (tracked[i].m == m) tracked[i].m = NULL;
    sceKernelUnlockMutex(music_mutex, 1);
}

void aw_music_stop(Music *m) {
    if (!started) { StopMusicStream(*m); return; }
    sceKernelLockMutex(music_mutex, 1, NULL);  // StopMusicStream rewinds the decoder: not during a decode
    StopMusicStream(*m);
    for (int i = 0; i < AW_MUSIC_MAX; i++)
        if (tracked[i].m == m) tracked[i].ending = 0;
    sceKernelUnlockMutex(music_mutex, 1);
}

void aw_music_seek(Music *m, float position) {
    if (!started) { SeekMusicStream(*m, position); return; }
    sceKernelLockMutex(music_mutex, 1, NULL);
    SeekMusicStream(*m, position);
    for (int i = 0; i < AW_MUSIC_MAX; i++)
        if (tracked[i].m == m) tracked[i].ending = 0;
    sceKernelUnlockMutex(music_mutex, 1);
}

int aw_sound_request(int id, const char *path) {
    if (!started) return 0;
    int free_slot = -1;
    for (int k = 0; k < AW_REQ_MAX; k++) {
        if (req[k].state != 0 && req[k].id == id) return 1;
        if (req[k].state == 0 && free_slot < 0) free_slot = k;
    }
    if (free_slot < 0 || strlen(path) >= sizeof(req[0].path)) return 0;
    Req *r = &req[free_slot];
    r->id = id;
    strcpy(r->path, path);
    __sync_synchronize();  // the path is complete before the loader thread can see state 1
    r->state = 1;
    sceKernelSignalSema(loader_sema, 1);
    return 1;
}

int aw_sound_poll(int *id, Sound *s, double *ms) {
    if (!started) return 0;
    for (int k = 0; k < AW_REQ_MAX; k++) {
        if (req[k].state != 2) continue;
        __sync_synchronize();
        *id = req[k].id, *s = req[k].s, *ms = req[k].ms;
        req[k].state = 0;
        return 1;
    }
    return 0;
}

#else  // desktop: no threads

void aw_start(void) {}
int aw_threaded(void) { return 0; }
void aw_music_track(Music *m) { (void)m; }
void aw_music_untrack(Music *m) { (void)m; }
void aw_music_stop(Music *m) { StopMusicStream(*m); }
int aw_sfx_play(Sound s, float volume, float pitch) { (void)s, (void)volume, (void)pitch; return 0; }
int aw_sfx_stop(Sound s) { (void)s; return 0; }
int aw_music_volume(Music *m, float volume) { (void)m, (void)volume; return 0; }
void aw_music_seek(Music *m, float position) { SeekMusicStream(*m, position); }
int aw_sound_request(int id, const char *path) { (void)id, (void)path; return 0; }
int aw_sound_poll(int *id, Sound *s, double *ms) { (void)id, (void)s, (void)ms; return 0; }

#endif
