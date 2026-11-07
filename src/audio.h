// Audio fuori dal thread principale: vedi audio.c
#pragma once
#include "raylib.h"

void au_start(void);  // dopo InitAudioDevice

// Effetti: le chiamate a raylib aspettano il mixer, qui il gioco accoda e prosegue
void au_sound_play(Sound s, float volume, float pitch);
void au_sound_stop(Sound s);

// Musica. Registrare il flusso dopo Play e Seek, toglierlo prima di chiuderlo o sostituirlo.
void au_music_add(Music *m);
void au_music_remove(Music *m);
void au_music_stop(Music *m);
void au_music_seek(Music *m, float seconds);
void au_music_volume(Music *m, float volume);
void au_music_update(Music *m);  // senza thread (nessun audio): lo fa il chiamante

// Primo uso di un effetto: 1 = in coda, 0 = lo carichi tu. Poi au_load_poll restituisce quelli pronti.
int au_load_request(int id, const char *path);
int au_load_poll(int *id, Sound *sound);
