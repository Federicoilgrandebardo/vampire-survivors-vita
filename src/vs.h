// Runtime comune: percorsi dei dati, atlanti, dati JSON, testi, salvataggio, scene.
#pragma once

#include "raylib.h"

// VS_DET=<seme>: prova di determinismo per lo studio del multiplayer (dt fisso, PRNG proprio, hash per frame in <dati>/det.txt)
extern int det_on;
extern unsigned det_frame, det_calls;
int det_rand(void);
void det_begin(unsigned seed);
void det_end(void);
#include "third_party/cJSON.h"

// Testo: con vs_font il glifo si trova da una tabella (raylib lo cerca in modo lineare, e il giapponese ne ha ~3000);
// con gli altri font sono le funzioni di raylib. Definite in runtime.c.
void vs_DrawTextEx(Font font, const char *text, Vector2 position, float fontSize, float spacing, Color tint);
Vector2 vs_MeasureTextEx(Font font, const char *text, float fontSize, float spacing);
// Miscela: dopo il cambio la draw call vuota torna QUADS. Con la patch lazy-blend della raylib Vita (5.5) un cambio di miscela
// dopo dei triangoli (DrawLineEx, cerchi) lascia una draw call TRIANGLES vuota; DrawTexturePro fa rlSetTexture e poi
// rlBegin(RL_QUADS), e rlBegin al cambio di modalita' rimette la texture di default: lo sprite usciva come quad bianco
// pieno (o nero, se tinto), es. la stella del colpo dopo la scia del Runetracer. Raylib 6 non ha il problema.
void vs_BeginBlendMode(int mode);
void vs_EndBlendMode(void);
#ifndef VS_RUNTIME_TEXT
#define DrawTextEx vs_DrawTextEx
#define MeasureTextEx vs_MeasureTextEx
#define BeginBlendMode vs_BeginBlendMode
#define EndBlendMode vs_EndBlendMode
#endif

#define SCREEN_W 960
#define SCREEN_H 544

// --- Percorsi ----------------------------------------------------------------
// I dati del gioco (tools/prepare.sh) stanno fuori dal VPK: ux0:data/VampireSurvivors/
// sulla Vita, $VS_DATA (o gamedata/) sul desktop.
void vs_init_paths(void);
const char *vs_path(const char *rel);  // buffer statico a rotazione: copiare se serve a lungo

// --- Video (runtime.c): sceAvPlayer sulla Vita, ffmpeg in pipe sul desktop -----
// Parte subito e va a tempo reale; update ritorna 0 a video finito (o se non si apre). w x h: le dimensioni del file (tools/prepare.sh).
typedef struct VsVideo VsVideo;
VsVideo *vs_video_open(const char *rel, int w, int h);
int vs_video_update(VsVideo *v, float ms);
Texture2D vs_video_texture(const VsVideo *v);
void vs_video_loop(VsVideo *v);        // ricomincia da solo alla fine
int vs_video_ready(const VsVideo *v);  // 1 quando c'e' almeno un fotogramma
const char *vs_asset(const char *rel); // file nel VPK (arte fatta per il port): app0:assets/ sulla Vita, accanto all'eseguibile sul desktop
void vs_video_close(VsVideo *v);

// --- Atlanti (TexturePacker di Phaser: img/<nome>.json + png) ------------------
typedef struct {
    Texture2D *tex;
    short x, y, w, h;    // rettangolo nell'atlante
    short ox, oy;        // posizione del ritaglio dentro la cornice originale
    short sw, sh;        // cornice originale
} Frame;

int atlas_load(const char *name);                              // 0 se fallisce
const Frame *frame_get(const char *atlas, const char *frame);   // NULL se manca
int atlas_add_image(const char *atlas, const char *frame, const char *path);  // un PNG intero come fotogramma (0 se manca)
cJSON *lang_table(const char *file);  // lang/<file>.json gia' caricato (lingua -> translations -> ID)
void frame_draw(const Frame *f, Vector2 center, int flip, float scale, Color tint);
void frame_draw_ex(const Frame *f, Vector2 center, int flip, float sx, float sy, float angle, Color tint);

// --- Dati e testi -------------------------------------------------------------
cJSON *data_get(const char *name);  // data/<nome>.json, caricato una volta
cJSON *read_json(const char *rel);  // un JSON qualsiasi dei dati (NULL se manca); lo libera il chiamante
int lang_load(const char *code);    // lang/lang.json, traduzioni di una lingua
const char *tr(const char *key);    // testo tradotto, o la chiave se manca
const char *tr_of(const char *file, const char *id, const char *field);  // es. ("characterLang", "ANTONIO", "charName")
extern Font vs_font;

// --- Salvataggio --------------------------------------------------------------
// PlayerOptions (0x17895) e SaveSystem (0xeea8) stanno in vs_game.c; qui i file e lo sha256.
extern cJSON *PlayerOptions;          // Core.PlayerOptions: lo stesso oggetto JSON dell'originale
void SaveSystem_Load(void);           // crea PlayerOptions coi default e applica SaveData se valido
void PlayerOptions_Save(void);        // PlayerOptions.Save -> SaveSystem.Save
char *vs_read_text(const char *rel);  // NULL se manca; si libera con vs_free
void vs_free(void *p);
int vs_write_text(const char *rel, const char *s);
void sha256_hex(const char *msg, char out[65]);
void Lang_init(void);                 // Lang.init: lingua salvata, del sistema o 'en'
void Lang_setLangCode(const char *code);
const char *Lang_getLangCode(void);
void vs_load_font(void);              // vs_font per la lingua corrente (ANY, KO e gli ideogrammi della console)
void Trophies_sync(void);
void vs_delete_all_and_exit(void);  // cancella la cartella dei dati e chiude il gioco
void vs_reload(void);                // window.location.reload: il gioco riparte da capo
const char *Lang_codeAt(int i);       // langKeys.json, NULL oltre la fine
const char *Lang_native(const char *code);

// --- Scene --------------------------------------------------------------------
typedef enum { SCENE_MENU, SCENE_GAME } Scene;
void scene_set(Scene s);

// Menu (vs_game.c): IntroScene e scene seguenti; menu_update = 1 quando parte la partita, -1 per uscire
void menu_start(void);
int menu_update(float dt);
void menu_draw(void);

// Partita (vs_game.c)
typedef struct {
    int seconds, kills, level;
    char character[32];
} Result;
void game_start(const char *character);
const char *game_prof(void);
int vs_quality_mode(void);  // Opzioni: profilo Qualita' (attesa GPU, niente lampi) invece di Prestazioni (vs_game.c)
int vs_finish_mode(void);  // VS_FINISH: 1 = rt, 2 = swap, 3 = both (main.c)
void vs_gl_finish(void);  // VS_PROF: conteggi della scena di gioco
int game_update(float dt);  // 0 finche' la partita continua
void game_draw(void);
extern Result last_result;

// Input comune: tasto di conferma, annulla, direzioni (pad o tastiera)
int in_confirm(void);
int in_cancel(void);
int in_dir_pressed(int *dx, int *dy);
Vector2 in_move(void);
