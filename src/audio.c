// Audio fuori dal thread principale. Idea di DrakonPL: su Vita il mixer di raylib tiene un lucchetto globale mentre mescola,
// e UpdateMusicStream decodifica con lo stesso lucchetto in mano, quindi il gioco perdeva millisecondi a ogni frame.
// Tre thread, con solo chiamate a raylib: qui malloc e' quello vero, non gmem (che non regge i thread).
//   musica  decodifica i flussi registrati; sulla Vita fuori dal lucchetto
//   comandi esegue play, stop e volume, che aspettano il mixer
//   carica  legge gli effetti al primo uso
#include <stdio.h>
#include <string.h>
#include "raylib.h"
#include "audio.h"

#ifdef __vita__
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
typedef SceUID AuMutex;
static void au_mutex_init(AuMutex *m) { *m = sceKernelCreateMutex("vs_audio", 0, 0, NULL); }
static void au_lock(AuMutex *m) { sceKernelLockMutex(*m, 1, NULL); }
static void au_unlock(AuMutex *m) { sceKernelUnlockMutex(*m, 1); }
static void au_sleep_us(int us) { sceKernelDelayThread(us); }
#define AU_THREAD(name) static int name(SceSize args_, void *argp_)
#define AU_THREAD_END (void)args_, (void)argp_; return 0
// Priorita': numero piu' basso = prima. I comandi aspettano solo il mixer; la musica non deve restare senza dati; il caricamento viene dopo il gioco.
static void au_thread_start(const char *name, SceKernelThreadEntry fn, int prio_delta, int core_mask, int stack) {
    SceUID t = sceKernelCreateThread(name, fn, 0x10000100 + prio_delta, stack, 0, core_mask, NULL);
    if (t >= 0) sceKernelStartThread(t, 0, NULL);
}
// Il decoder dentro libraylib.a, aperto da LoadMusicStream (Music.ctxData)
typedef struct stb_vorbis stb_vorbis;
int stb_vorbis_get_samples_short_interleaved(stb_vorbis *f, int channels, short *buffer, int num_shorts);
int stb_vorbis_seek_start(stb_vorbis *f);
#define AU_MUSIC_OGG 2  // raudio.c: NONE, WAV, OGG, ...
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_mutex_t AuMutex;
static void au_mutex_init(AuMutex *m) { pthread_mutex_init(m, NULL); }
static void au_lock(AuMutex *m) { pthread_mutex_lock(m); }
static void au_unlock(AuMutex *m) { pthread_mutex_unlock(m); }
static void au_sleep_us(int us) { usleep(us); }
#define AU_THREAD(name) static void *name(void *arg_)
#define AU_THREAD_END (void)arg_; return NULL
static void au_thread_start(const char *name, void *(*fn)(void *), int prio_delta, int core_mask, int stack) {
    (void)name, (void)prio_delta, (void)core_mask, (void)stack;
    pthread_t t;
    pthread_create(&t, NULL, fn, NULL);
}
#define SCE_KERNEL_CPU_MASK_USER_1 0
#define SCE_KERNEL_CPU_MASK_USER_2 0
#endif

static int started;

// --- Comandi: un solo scrittore (il gioco) e un solo lettore, nessun lucchetto ------------------------------------------
enum { AU_PLAY, AU_STOP, AU_VOLUME };
typedef struct { int op; AudioStream stream; float volume, pitch; } AuCmd;
#define AU_CMDS 256
static AuCmd cmds[AU_CMDS];
static volatile unsigned cmd_head, cmd_tail;
static void au_cmd_run(const AuCmd *c) {
    if (c->op == AU_STOP) StopAudioStream(c->stream);
    else if (c->op == AU_VOLUME) SetAudioStreamVolume(c->stream, c->volume);
    else SetAudioStreamVolume(c->stream, c->volume), SetAudioStreamPitch(c->stream, c->pitch), PlayAudioStream(c->stream);
}
static void au_cmd_push(int op, AudioStream stream, float volume, float pitch) {
    AuCmd c = { op, stream, volume, pitch };
    if (!started || cmd_head - cmd_tail >= AU_CMDS) { au_cmd_run(&c); return; }  // coda piena: si aspetta il mixer, ma il comando non si perde
    cmds[cmd_head % AU_CMDS] = c;
    __sync_synchronize();
    cmd_head++;
}
AU_THREAD(au_cmd_thread) {
    for (;;) {
        while (cmd_tail != cmd_head) {
            __sync_synchronize();
            AuCmd c = cmds[cmd_tail % AU_CMDS];
            au_cmd_run(&c);
            __sync_synchronize();
            cmd_tail++;
        }
        au_sleep_us(1000);
    }
    AU_THREAD_END;
}
void au_sound_play(Sound s, float volume, float pitch) { au_cmd_push(AU_PLAY, s.stream, volume, pitch); }
void au_sound_stop(Sound s) { au_cmd_push(AU_STOP, s.stream, 0, 0); }
void au_music_volume(Music *m, float volume) { au_cmd_push(AU_VOLUME, m->stream, volume, 0); }

// --- Musica -------------------------------------------------------------------------------------------------------------
#define AU_MUSICS 8
#define AU_CHUNK 4096  // frame per meta' buffer: 93 ms a 44,1 kHz
static AuMutex music_lock;
static struct { Music *m; int ending; } musics[AU_MUSICS];  // ending: 1 ultimi frame in coda, 2 anche un pezzo di silenzio dopo di loro
#ifdef __vita__
static short pcm[AU_CHUNK * 2];
static void au_music_fill(int t) {
    Music m = *musics[t].m;
    if (m.ctxType != AU_MUSIC_OGG || m.stream.sampleSize != 16 || m.stream.channels > 2) { UpdateMusicStream(m); return; }
    while (IsAudioStreamProcessed(m.stream)) {
        if (musics[t].ending == 2) { StopMusicStream(m); musics[t].ending = 0; return; }
        if (musics[t].ending == 1) { UpdateAudioStream(m.stream, pcm, 0); musics[t].ending = 2; continue; }  // raylib riempie di zeri
        int ch = m.stream.channels, got = 0, rewound = 0;
        while (got < AU_CHUNK) {
            int n = stb_vorbis_get_samples_short_interleaved((stb_vorbis *)m.ctxData, ch, pcm + got * ch, (AU_CHUNK - got) * ch);
            if (n > 0) { got += n, rewound = 0; continue; }
            if (!musics[t].m->looping || rewound) break;  // fine, o un file senza campioni
            stb_vorbis_seek_start((stb_vorbis *)m.ctxData), rewound = 1;
        }
        UpdateAudioStream(m.stream, pcm, got);
        if (got < AU_CHUNK) musics[t].ending = 1;
    }
}
#else
static void au_music_fill(int t) { UpdateMusicStream(*musics[t].m); }  // sul desktop il decoder non e' esportato: la strada di raylib
#endif
AU_THREAD(au_music_thread) {
    for (;;) {
        au_lock(&music_lock);
        for (int i = 0; i < AU_MUSICS; i++) if (musics[i].m) au_music_fill(i);
        au_unlock(&music_lock);
        au_sleep_us(5000);  // un buffer dura 186 ms
    }
    AU_THREAD_END;
}
void au_music_add(Music *m) {
    if (!started) return;
    au_lock(&music_lock);
    int slot = -1;
    for (int i = 0; i < AU_MUSICS && slot < 0; i++) if (musics[i].m == m) slot = i;  // gia' in lista: suonato di nuovo dopo uno stop
    for (int i = 0; i < AU_MUSICS && slot < 0; i++) if (!musics[i].m) slot = i;
    if (slot >= 0) musics[slot].m = m, musics[slot].ending = 0;
    au_unlock(&music_lock);
}
void au_music_remove(Music *m) {  // prima di chiudere o sostituire il flusso
    if (!started) return;
    while (cmd_tail != cmd_head) au_sleep_us(200);  // un comando in coda puo' nominare questo flusso
    au_lock(&music_lock);
    for (int i = 0; i < AU_MUSICS; i++) if (musics[i].m == m) musics[i].m = NULL;
    au_unlock(&music_lock);
}
static void au_music_rewound(Music *m) { for (int i = 0; i < AU_MUSICS; i++) if (musics[i].m == m) musics[i].ending = 0; }
void au_music_stop(Music *m) {
    if (!started) { StopMusicStream(*m); return; }
    au_lock(&music_lock);  // sposta il decoder: mai durante una decodifica
    StopMusicStream(*m);
    au_music_rewound(m);
    au_unlock(&music_lock);
}
void au_music_seek(Music *m, float seconds) {
    if (!started) { SeekMusicStream(*m, seconds); return; }
    au_lock(&music_lock);
    SeekMusicStream(*m, seconds);
    au_music_rewound(m);
    au_unlock(&music_lock);
}
void au_music_update(Music *m) { if (!started) UpdateMusicStream(*m); }

// --- Caricamento --------------------------------------------------------------------------------------------------------
#define AU_LOADS 16
static struct { volatile int state; int id; char path[256]; Sound sound; } loads[AU_LOADS];  // 0 libero, 1 in coda, 2 pronto
AU_THREAD(au_load_thread) {
    for (;;) {
        for (int k = 0; k < AU_LOADS; k++) {
            if (loads[k].state != 1) continue;
            loads[k].sound = LoadSound(loads[k].path);
            __sync_synchronize();
            loads[k].state = 2;
        }
        au_sleep_us(5000);
    }
    AU_THREAD_END;
}
int au_load_request(int id, const char *path) {
    if (!started || strlen(path) >= sizeof(loads[0].path)) return 0;
    for (int k = 0; k < AU_LOADS; k++) {
        if (loads[k].state != 0) continue;
        loads[k].id = id;
        strcpy(loads[k].path, path);
        __sync_synchronize();
        loads[k].state = 1;
        return 1;
    }
    return 0;  // coda piena: lo carica chi ha chiesto
}
int au_load_poll(int *id, Sound *sound) {
    for (int k = 0; started && k < AU_LOADS; k++) {
        if (loads[k].state != 2) continue;
        __sync_synchronize();
        *id = loads[k].id, *sound = loads[k].sound;
        loads[k].state = 0;
        return 1;
    }
    return 0;
}

void au_start(void) {
    if (started || !IsAudioDeviceReady()) return;
    au_mutex_init(&music_lock);
#ifdef __vita__
    SetAudioStreamBufferSizeDefault(AU_CHUNK);  // prima di ogni LoadMusicStream: au_music_fill scrive meta' buffer intere
#endif
    started = 1;
    au_thread_start("vs_audio_cmd", au_cmd_thread, -20, SCE_KERNEL_CPU_MASK_USER_1, 64 * 1024);
    au_thread_start("vs_music", au_music_thread, -10, SCE_KERNEL_CPU_MASK_USER_1, 256 * 1024);
    au_thread_start("vs_load", au_load_thread, 10, SCE_KERNEL_CPU_MASK_USER_2, 256 * 1024);
}
