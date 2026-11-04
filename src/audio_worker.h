// Audio worker (PS Vita): three threads on other CPU cores do the slow audio work of the main thread.
// - The music thread decodes every tracked music stream, without the audio lock of raylib (music_fill).
// - The loader thread loads a sound effect on its first use (a load can take 70 ms on the Vita).
// - The sound thread runs the raylib calls of sound effects and music volume, which wait for the mixer.
// On the desktop no thread starts: aw_threaded() is 0, and the game does the work itself, as before.
#pragma once
#include "raylib.h"

void aw_start(void);     // after InitAudioDevice. Vita only: starts the threads
int aw_threaded(void);   // 1 when the threads run

// Music streams. Track a stream after its load, untrack it before its unload. Untrack waits until the music
// thread and the sound thread no longer use the stream. raylib's own audio lock protects all the other calls on the stream.
void aw_music_track(Music *m);
void aw_music_untrack(Music *m);
// Stop and seek of a music stream: they move the decoder, so on the Vita they wait for the end of a decode
void aw_music_stop(Music *m);
void aw_music_seek(Music *m, float position);

// Sound effects: on the Vita the sound thread runs the raylib calls, so the main thread never waits for the mixer.
// Return 1 when the command is queued, 0 when the caller must do the call itself (desktop).
int aw_sfx_play(Sound s, float volume, float pitch);  // SetSoundVolume, SetSoundPitch, PlaySound
int aw_sfx_stop(Sound s);                             // StopSound
int aw_music_volume(Music *m, float volume);          // SetMusicVolume (the fades of the music)

// First loads of sound effects. Returns 1 when the load is queued (or already queued), 0 when the queue is full.
int aw_sound_request(int id, const char *path);
// A sound that the loader thread finished: returns 1 and fills id, the sound and the load time. Call it until it
// returns 0.
int aw_sound_poll(int *id, Sound *s, double *ms);
