#include <dirent.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VS_RUNTIME_TEXT  // qui DrawTextEx e MeasureTextEx sono quelle di raylib
#include "vs.h"
#include "rlgl.h"

void vs_BeginBlendMode(int mode) { BeginBlendMode(mode); rlBegin(RL_QUADS); rlEnd(); }  // vedi vs.h
void vs_EndBlendMode(void) { EndBlendMode(); rlBegin(RL_QUADS); rlEnd(); }

#ifdef __vita__
#include <malloc.h>
#include <psp2/apputil.h>
#include <psp2/avplayer.h>
#include <psp2/gxm.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/sysmodule.h>
#include <psp2/system_param.h>
#include <vitaGL.h>
#endif

// --- Percorsi ----------------------------------------------------------------
static char data_dir[512];

void vs_init_paths(void) {
#ifdef __vita__
    snprintf(data_dir, sizeof(data_dir), "ux0:data/VampireSurvivors/");
#else
    const char *e = getenv("VS_DATA");
    snprintf(data_dir, sizeof(data_dir), "%s/", e && *e ? e : "gamedata");
#endif
}

static void delete_tree(const char *path) {
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char child[600];
            snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            struct stat st;
#ifdef __vita__
            int ok = stat(child, &st);
#else
            int ok = lstat(child, &st);  // a link is removed, never followed
#endif
            if (!ok && S_ISDIR(st.st_mode)) delete_tree(child);
            else remove(child);
        }
        closedir(d);
    }
    rmdir(path);
}

void vs_delete_all_and_exit(void) {
    char root[512];
    const char *dev = getenv("VS_DELETE_ROOT");  // only to test the routine on a throwaway folder
    snprintf(root, sizeof(root), "%s", dev && *dev ? dev : data_dir);
    size_t n = strlen(root);
    if (n && root[n - 1] == '/') root[n - 1] = 0;
#ifndef __vita__
    if (!dev) { TraceLog(LOG_WARNING, "VS: delete all is disabled on the desktop"); exit(0); }
#endif
    delete_tree(root);
    exit(0);
}

const char *vs_path(const char *rel) {
    static char buf[8][600];
    static int i;
    char *b = buf[i++ & 7];
    snprintf(b, sizeof(buf[0]), "%s%s", data_dir, rel);
    return b;
}

static char *read_text(const char *rel) {
    int n = 0;
    unsigned char *d = LoadFileData(vs_path(rel), &n);
    if (!d)
        return NULL;
    char *s = malloc(n + 1);
    memcpy(s, d, n);
    s[n] = 0;
    UnloadFileData(d);
    return s;
}

cJSON *read_json(const char *rel) {
    char *s = read_text(rel);
    if (!s)
        return NULL;
    cJSON *j = cJSON_Parse(s);
    free(s);
    if (!j)
        TraceLog(LOG_WARNING, "VS: JSON non valido: %s", rel);
    return j;
}

// --- Atlanti -----------------------------------------------------------------
// Tutti i frame di tutti gli atlanti in una tabella hash (atlante + nome frame).
#define MAX_ATLASES 64
#define MAX_PAGES 8
#define HASH_SIZE 65536  // potenza di 2, ben sopra i frame del gioco

typedef struct {
    char name[48];
    Texture2D pages[MAX_PAGES];
    int n_pages;
} Atlas;

typedef struct {
    unsigned atlas;  // indice + 1, 0 = vuoto
    char *name;
    Frame f;
} Slot;

static Atlas atlases[MAX_ATLASES];
static int n_atlases;
static Slot *slots;

static unsigned hash(unsigned atlas, const char *s) {
    unsigned h = 2166136261u ^ atlas;
    while (*s)
        h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

static int atlas_index(const char *name) {
    for (int i = 0; i < n_atlases; i++)
        if (!strcmp(atlases[i].name, name))
            return i;
    return -1;
}

static void add_frame(int ai, Texture2D *tex, cJSON *fr, const char *name) {
    cJSON *r = cJSON_GetObjectItem(fr, "frame"), *s = cJSON_GetObjectItem(fr, "spriteSourceSize"),
          *ss = cJSON_GetObjectItem(fr, "sourceSize");
    if (!r)
        return;
    if (cJSON_IsTrue(cJSON_GetObjectItem(fr, "rotated")))
        TraceLog(LOG_WARNING, "VS: frame ruotato non gestito: %s/%s", atlases[ai].name, name);
    Frame f = { tex,
                cJSON_GetObjectItem(r, "x")->valueint, cJSON_GetObjectItem(r, "y")->valueint,
                cJSON_GetObjectItem(r, "w")->valueint, cJSON_GetObjectItem(r, "h")->valueint,
                0, 0, 0, 0 };
    // Come Phaser (Textures/parsers/JSONHash): il ritaglio vale solo con trimmed: true.
    // Molti frame hanno trimmed false con uno spriteSourceSize spostato, da ignorare.
    if (cJSON_IsTrue(cJSON_GetObjectItem(fr, "trimmed"))) {
        f.ox = s ? cJSON_GetObjectItem(s, "x")->valueint : 0;
        f.oy = s ? cJSON_GetObjectItem(s, "y")->valueint : 0;
        f.sw = ss ? cJSON_GetObjectItem(ss, "w")->valueint : f.w;
        f.sh = ss ? cJSON_GetObjectItem(ss, "h")->valueint : f.h;
    } else {
        f.sw = f.w;
        f.sh = f.h;
    }
    unsigned h = hash(ai + 1, name);
    for (unsigned k = 0; k < HASH_SIZE; k++) {
        Slot *sl = &slots[(h + k) & (HASH_SIZE - 1)];
        if (!sl->atlas || (sl->atlas == (unsigned)ai + 1 && !strcmp(sl->name, name))) {
            if (!sl->atlas)
                sl->name = strdup(name);
            sl->atlas = ai + 1;
            sl->f = f;
            return;
        }
    }
    TraceLog(LOG_ERROR, "VS: tabella dei frame piena");
}

int atlas_load(const char *name) {
    if (atlas_index(name) >= 0)
        return 1;
    if (n_atlases == MAX_ATLASES)
        return 0;
    if (!slots)
        slots = calloc(HASH_SIZE, sizeof(Slot));
    char rel[128];
    snprintf(rel, sizeof(rel), "img/%s.json", name);
    cJSON *j = read_json(rel);
    if (!j)
        return 0;
    int ai = n_atlases++;
    Atlas *a = &atlases[ai];
    snprintf(a->name, sizeof(a->name), "%s", name);
    // Formato multi-pagina ("textures": [{image, frames: [...]}]) o hash semplice ("frames": {...}).
    cJSON *pages = cJSON_GetObjectItem(j, "textures"), *page;
    if (pages) {
        cJSON_ArrayForEach(page, pages) {
            if (a->n_pages == MAX_PAGES)
                break;
            const char *img = cJSON_GetStringValue(cJSON_GetObjectItem(page, "image"));
            snprintf(rel, sizeof(rel), "img/%s", img ? img : "");
            Texture2D *t = &a->pages[a->n_pages++];
            *t = LoadTexture(vs_path(rel));
            cJSON *fr;
            cJSON_ArrayForEach(fr, cJSON_GetObjectItem(page, "frames"))
                add_frame(ai, t, fr, cJSON_GetStringValue(cJSON_GetObjectItem(fr, "filename")));
        }
    } else {
        cJSON *meta = cJSON_GetObjectItem(j, "meta");
        const char *img = meta ? cJSON_GetStringValue(cJSON_GetObjectItem(meta, "image")) : NULL;
        snprintf(rel, sizeof(rel), "img/%s", img ? img : "");
        if (!img)
            snprintf(rel, sizeof(rel), "img/%s.png", name);
        Texture2D *t = &a->pages[a->n_pages++];
        *t = LoadTexture(vs_path(rel));
        cJSON *fr;
        cJSON_ArrayForEach(fr, cJSON_GetObjectItem(j, "frames")) {
            const char *fn = cJSON_GetStringValue(cJSON_GetObjectItem(fr, "filename"));
            add_frame(ai, t, fr, fn ? fn : fr->string);
        }
    }
    cJSON_Delete(j);
    return 1;
}

const Frame *frame_get(const char *atlas, const char *frame) {
    if (!atlas || !frame) return NULL;  // dati senza texture/frameName: fotogramma mancante, come in Phaser
    int ai = atlas_index(atlas);
    if (ai < 0 && (!atlas_load(atlas) || (ai = atlas_index(atlas)) < 0))
        return NULL;
    unsigned h = hash(ai + 1, frame);
    for (unsigned k = 0; k < HASH_SIZE; k++) {
        Slot *sl = &slots[(h + k) & (HASH_SIZE - 1)];
        if (!sl->atlas)
            return NULL;
        if (sl->atlas == (unsigned)ai + 1 && !strcmp(sl->name, frame))
            return &sl->f;
    }
    return NULL;
}

// Centro della cornice originale in c (origine 0.5 come in Phaser).
void frame_draw_ex(const Frame *f, Vector2 c, int flip, float sx, float sy, float angle, Color tint) {
    if (!f)
        return;
    Rectangle src = { f->x, f->y, flip ? -f->w : f->w, f->h };
    float ox = flip ? f->sw - f->ox - f->w : f->ox;
    // origine = centro della cornice originale, cosi' la rotazione gira attorno a c
    Vector2 org = { (f->sw * 0.5f - ox) * sx, (f->sh * 0.5f - f->oy) * sy };
    DrawTexturePro(*f->tex, src, (Rectangle){ c.x, c.y, f->w * sx, f->h * sy }, org, angle, tint);
}

void frame_draw(const Frame *f, Vector2 c, int flip, float scale, Color tint) {
    frame_draw_ex(f, c, flip, scale, scale, 0, tint);
}

// --- Dati e testi -------------------------------------------------------------
cJSON *data_get(const char *name) {
    static struct { char name[32]; cJSON *j; } cache[32];
    for (int i = 0; i < 32; i++) {
        if (!cache[i].j) {
            char rel[64];
            snprintf(rel, sizeof(rel), "data/%s.json", name);
            snprintf(cache[i].name, sizeof(cache[i].name), "%s", name);
            cache[i].j = read_json(rel);
            return cache[i].j;
        }
        if (!strcmp(cache[i].name, name))
            return cache[i].j;
    }
    return NULL;
}

static cJSON *lang_all, *lang_cur, *lang_en;
static char lang_code[8] = "en";
Font vs_font;

int lang_load(const char *code) {
    if (!lang_all)
        lang_all = read_json("lang/lang.json");
    if (!lang_all)
        return 0;
    cJSON *l = cJSON_GetObjectItem(lang_all, code);
    lang_cur = l ? cJSON_GetObjectItem(l, "translations") : NULL;
    lang_en = cJSON_GetObjectItem(cJSON_GetObjectItem(lang_all, "en"), "translations");
    snprintf(lang_code, sizeof(lang_code), "%s", lang_cur ? code : "en");
    return lang_cur != NULL;
}

// Testi delle entita' (lang/<file>.json: lingua -> translations -> ID -> campo).
// I file restano in memoria interi: cambiare lingua non li ricarica.
static cJSON *lang_file(const char *file) {
    static struct { char file[32]; cJSON *j; } cache[16];
    for (int i = 0; i < 16; i++) {
        if (!cache[i].j) {
            char rel[64];
            snprintf(rel, sizeof(rel), "lang/%s.json", file);
            snprintf(cache[i].file, sizeof(cache[i].file), "%s", file);
            return cache[i].j = read_json(rel);
        }
        if (!strcmp(cache[i].file, file)) return cache[i].j;
    }
    return NULL;
}
const char *tr_of(const char *file, const char *id, const char *field) {
    cJSON *j = lang_file(file);
    const char *codes[] = { lang_code, "en" };
    for (int c = 0; c < 2 && j; c++) {
        cJSON *t = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(j, codes[c]), "translations"), id);
        const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(t, field));
        if (s && *s)
            return s;
    }
    return "";
}

const char *tr(const char *key) {
    const char *s = lang_cur ? cJSON_GetStringValue(cJSON_GetObjectItem(lang_cur, key)) : NULL;
    if (!s && lang_en)
        s = cJSON_GetStringValue(cJSON_GetObjectItem(lang_en, key));
    return s ? s : key;
}

// Lang.init / setLangCode (Lang): PlayerOptions.Language, se no la lingua del sistema, se no 'en'.
// Integration.getLanguage restituisce il nome "api" di Steam (italian, schinese...): qui quello della console.
static const char *Integration_getLanguage(void) {
#ifdef __vita__
    static const char *const api[] = { "japanese", "english", "french", "spanish", "german", "italian", "dutch", "portuguese", "russian",
                                       "koreana", "tchinese", "schinese", "finnish", "swedish", "danish", "norwegian", "polish", "brazilian",
                                       "english", "turkish" };  // SCE_SYSTEM_PARAM_LANG_*
    SceAppUtilInitParam ip = { 0 };
    SceAppUtilBootParam bp = { 0 };
    sceAppUtilInit(&ip, &bp);  // gia' fatto da SDL: l'errore si ignora
    int l = -1;
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &l);
    return l >= 0 && l < (int)(sizeof(api) / sizeof(*api)) ? api[l] : NULL;
#else
    static char code[8];  // LANG=it_IT.UTF-8 -> "it" -> lang.json["it"].api
    const char *e = getenv("LANG");
    if (!e || !lang_all) return NULL;
    snprintf(code, sizeof(code), "%.*s", (int)strcspn(e, "_."), e);
    return cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(lang_all, code), "api"));
#endif
}

const char *Lang_getLangCode(void) { return lang_code; }
const char *Lang_codeAt(int i) {  // Lang.langCodeList (langKeys.json), NULL oltre la fine
    static cJSON *keys;
    if (!keys) keys = read_json("lang/langKeys.json");
    return cJSON_GetStringValue(cJSON_GetArrayItem(keys, i));
}
const char *Lang_native(const char *code) {  // Lang.langNameList: lang.json[code].native
    if (!lang_all) lang_all = read_json("lang/lang.json");
    const char *n = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(lang_all, code), "native"));
    return n ? n : code;
}

void Lang_setLangCode(const char *code) {
    lang_load(code);
    const char *cur = cJSON_GetStringValue(cJSON_GetObjectItem(PlayerOptions, "Language"));
    if (!cur || strcmp(cur, lang_code)) {
        cJSON_ReplaceItemInObject(PlayerOptions, "Language", cJSON_CreateString(lang_code));
        PlayerOptions_Save();  // SaveOptions
    }
}

void Lang_init(void) {
    if (!lang_all) lang_all = read_json("lang/lang.json");
    const char *api = Integration_getLanguage(), *sys = NULL;
    cJSON *keys = read_json("lang/langKeys.json"), *k;
    cJSON_ArrayForEach(k, keys) {
        const char *a = cJSON_IsString(k) ? cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(lang_all, k->valuestring), "api")) : NULL;
        if (api && a && !strcmp(api, a)) { sys = k->valuestring; break; }
    }
    static char code[8];
    const char *po = cJSON_GetStringValue(cJSON_GetObjectItem(PlayerOptions, "Language"));
    snprintf(code, sizeof(code), "%s", po && *po ? po : sys ? sys : "en");
    if (getenv("VS_LANG")) snprintf(code, sizeof(code), "%s", getenv("VS_LANG"));  // sviluppo: la lingua di prova (si salva)
    cJSON_Delete(keys);
    Lang_setLangCode(code);
}

// --- Salvataggio --------------------------------------------------------------
// File di testo nella cartella dei dati, scritto prima su un temporaneo: un salvataggio interrotto non cancella quello buono.
char *vs_read_text(const char *rel) { return read_text(rel); }
void vs_free(void *p) { free(p); }  // vs_game.c ridefinisce free (gmem)

int vs_write_text(const char *rel, const char *s) {
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", vs_path(rel));
    if (!SaveFileText(tmp, (char *)s)) return 0;
#ifdef __vita__
    remove(vs_path(rel));  // sceIoRename non sovrascrive
#endif
    return rename(tmp, vs_path(rel)) == 0;
}

// SHA-256 (FIPS 180-4) in esadecimale minuscolo, come createHash('sha256').digest('hex')
void sha256_hex(const char *msg, char out[65]) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))
    size_t len = strlen(msg), total = ((len + 9 + 63) / 64) * 64;
    for (size_t off = 0; off < total; off += 64) {
        uint8_t blk[64];
        for (int i = 0; i < 64; i++) {
            size_t p = off + i;
            blk[i] = p < len ? (uint8_t)msg[p] : p == len ? 0x80 : 0;
        }
        if (off + 64 == total)
            for (int i = 0; i < 8; i++) blk[63 - i] = (uint8_t)((uint64_t)len * 8 >> (8 * i));
        uint32_t w[64], a[8];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)blk[4 * i] << 24 | blk[4 * i + 1] << 16 | blk[4 * i + 2] << 8 | blk[4 * i + 3];
        for (int i = 16; i < 64; i++)
            w[i] = w[i - 16] + (ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ w[i - 15] >> 3) + w[i - 7] + (ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ w[i - 2] >> 10);
        memcpy(a, h, sizeof(a));
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = a[7] + (ROR(a[4], 6) ^ ROR(a[4], 11) ^ ROR(a[4], 25)) + ((a[4] & a[5]) ^ (~a[4] & a[6])) + K[i] + w[i];
            uint32_t t2 = (ROR(a[0], 2) ^ ROR(a[0], 13) ^ ROR(a[0], 22)) + ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
            memmove(a + 1, a, 7 * sizeof(*a));
            a[4] += t1, a[0] = t1 + t2;
        }
        for (int i = 0; i < 8; i++) h[i] += a[i];
    }
#undef ROR
    for (int i = 0; i < 8; i++) snprintf(out + 8 * i, 9, "%08x", (unsigned)h[i]);
}

// --- Input --------------------------------------------------------------------
// Sulla Vita: croce = conferma, cerchio = annulla (layout occidentale, come VS su Switch).
int in_confirm(void) {
    return IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE) ||
           (IsGamepadAvailable(0) && (IsGamepadButtonPressed(0, GAMEPAD_BUTTON_RIGHT_FACE_DOWN) ||
                                      IsGamepadButtonPressed(0, GAMEPAD_BUTTON_MIDDLE_RIGHT)));
}

int in_cancel(void) {
    return IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_BACKSPACE) ||
           (IsGamepadAvailable(0) && IsGamepadButtonPressed(0, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT));
}

int in_dir_pressed(int *dx, int *dy) {
    *dx = *dy = 0;
    int pad = IsGamepadAvailable(0);
    if (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A) || (pad && IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_LEFT))) *dx = -1;
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D) || (pad && IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_RIGHT))) *dx = 1;
    if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W) || (pad && IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_UP))) *dy = -1;
    if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S) || (pad && IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_DOWN))) *dy = 1;
    // Levetta: uno scatto quando supera la soglia.
    static int stick_held;
    if (pad) {
        float x = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_X), y = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_Y);
        int held = fabsf(x) > 0.6f || fabsf(y) > 0.6f;
        if (held && !stick_held) {
            if (fabsf(x) > fabsf(y)) *dx = x > 0 ? 1 : -1;
            else *dy = y > 0 ? 1 : -1;
        }
        stick_held = held;
    }
    return *dx || *dy;
}

Vector2 in_move(void) {
    Vector2 m = { 0, 0 };
    if (IsGamepadAvailable(0)) {
        m.x = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_X);
        m.y = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_Y);
        if (fabsf(m.x) < 0.2f) m.x = 0;
        if (fabsf(m.y) < 0.2f) m.y = 0;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_FACE_LEFT)) m.x = -1;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_FACE_RIGHT)) m.x = 1;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_FACE_UP)) m.y = -1;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_FACE_DOWN)) m.y = 1;
    }
    if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) m.x = -1;
    if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) m.x = 1;
    if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) m.y = -1;
    if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) m.y = 1;
    float l = sqrtf(m.x * m.x + m.y * m.y);
    if (l > 1) m.x /= l, m.y /= l;
    return m;
}

// --- Font --------------------------------------------------------------------
// Lang.FONTFAMILY: 'fontKO, fontANY, Courier, serif, sans-serif' per ko/ja/zh-CN, altrimenti 'Courier, fontANY, ...' (qui ANY al
// posto di Courier, come nell'HUD). Il browser prende ogni glifo dal primo font che ce l'ha, alla stessa dimensione e sulla linea
// di base del primo: qui si fa lo stesso una volta per lingua, con i soli glifi che i testi di quella lingua usano (piu' i nomi
// delle lingue, sempre nelle Opzioni). Gli ideogrammi non sono in nessun font del gioco: nel browser arrivano dal sistema, qui
// dai font della console (ScePvf) o, sul desktop, da Arial Unicode.
// La ricerca dei glifi di raylib e' lineare: con ~3000 glifi (giapponese) costa. Per vs_font c'e' la tabella glyph_idx,
// usata dalle copie di DrawTextEx / MeasureTextEx qui sotto (stessi calcoli di raylib 5.5, interlinea 2 come il default).
#define FONT_PX 32
static unsigned short *glyph_idx;  // codepoint -> indice in vs_font.glyphs, 0xffff = assente
static int glyph_q;                // l'indice di '?', come il ripiego di GetGlyphIndex
static void glyph_index_build(void) {
    if (!glyph_idx) glyph_idx = malloc(sizeof(*glyph_idx) * 0x10000);
    memset(glyph_idx, 0xff, sizeof(*glyph_idx) * 0x10000);
    for (int i = vs_font.glyphCount - 1; i >= 0; i--)  // al contrario: a parita' di codepoint vince il primo, come la ricerca lineare
        if ((unsigned)vs_font.glyphs[i].value < 0x10000) glyph_idx[vs_font.glyphs[i].value] = (unsigned short)i;
    glyph_q = glyph_idx['?'] != 0xffff ? glyph_idx['?'] : 0;
}
static int glyph_of(int cp) { return glyph_idx && cp >= 0 && cp < 0x10000 && glyph_idx[cp] != 0xffff ? glyph_idx[cp] : glyph_q; }
void vs_DrawTextEx(Font font, const char *text, Vector2 position, float fontSize, float spacing, Color tint) {
    if (font.glyphs != vs_font.glyphs || !glyph_idx) { DrawTextEx(font, text, position, fontSize, spacing, tint); return; }
    float sc = fontSize / font.baseSize, ox = 0, oy = 0, pad = (float)font.glyphPadding;
    for (const char *s = text; *s;) {
        int n = 0, cp = GetCodepointNext(s, &n), i = glyph_of(cp);
        s += n > 0 ? n : 1;
        if (cp == '\n') { oy += fontSize + 2, ox = 0; continue; }
        if (cp != ' ' && cp != '\t') {  // DrawTextCodepoint
            Rectangle r = font.recs[i];
            DrawTexturePro(font.texture, (Rectangle){ r.x - pad, r.y - pad, r.width + 2 * pad, r.height + 2 * pad },
                           (Rectangle){ position.x + ox + (font.glyphs[i].offsetX - pad) * sc, position.y + oy + (font.glyphs[i].offsetY - pad) * sc, (r.width + 2 * pad) * sc, (r.height + 2 * pad) * sc },
                           (Vector2){ 0, 0 }, 0, tint);
        }
        ox += (font.glyphs[i].advanceX ? font.glyphs[i].advanceX : font.recs[i].width) * sc + spacing;
    }
}
Vector2 vs_MeasureTextEx(Font font, const char *text, float fontSize, float spacing) {
    if (font.glyphs != vs_font.glyphs || !glyph_idx) return MeasureTextEx(font, text, fontSize, spacing);
    if (!text || !*text || !font.texture.id) return (Vector2){ 0, 0 };
    float w = 0, maxW = 0, h = fontSize;
    int bytes = 0, maxBytes = 0;
    for (const char *s = text; *s;) {
        int n = 0, cp = GetCodepointNext(s, &n), i = glyph_of(cp);
        s += n > 0 ? n : 1;
        bytes++;
        if (cp != '\n') w += font.glyphs[i].advanceX > 0 ? font.glyphs[i].advanceX : font.recs[i].width + font.glyphs[i].offsetX;
        else { if (maxW < w) maxW = w; bytes = 0, w = 0, h += fontSize + 2; }
        if (maxBytes < bytes) maxBytes = bytes;
    }
    if (maxW < w) maxW = w;
    return (Vector2){ maxW * fontSize / font.baseSize + (maxBytes - 1) * spacing, h };
}
typedef struct { unsigned char *d; int n, base; unsigned char *has; } TTF;
static unsigned rd16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }
static unsigned rd32(const unsigned char *p) { return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3]; }
static const unsigned char *ttf_table(const TTF *t, const char *tag) {
    for (unsigned i = 0, nt = rd16(t->d + 4); i < nt; i++) {
        const unsigned char *r = t->d + 12 + 16 * i;
        if (!memcmp(r, tag, 4)) return rd32(r + 8) < (unsigned)t->n ? t->d + rd32(r + 8) : NULL;
    }
    return NULL;
}
static void cp_set(unsigned char *b, unsigned c) { if (c < 0x10000) b[c >> 3] |= 1 << (c & 7); }
static int cp_has(const unsigned char *b, unsigned c) { return c < 0x10000 && (b[c >> 3] >> (c & 7) & 1); }
static int ttf_open(TTF *t, const char *path) {  // cmap (formati 4 e 12) e linea di base come stbtt: ascent * px / (ascent - descent)
    memset(t, 0, sizeof(*t));
    t->d = FileExists(path) ? LoadFileData(path, &t->n) : NULL;
    const unsigned char *c = t->d ? ttf_table(t, "cmap") : NULL, *h = t->d ? ttf_table(t, "hhea") : NULL;
    if (!c || !h) { if (t->d) UnloadFileData(t->d); t->d = NULL; return 0; }
    t->has = calloc(0x2000, 1);
    for (unsigned i = 0, ns = rd16(c + 2); i < ns; i++) {
        const unsigned char *s = c + rd32(c + 8 + 8 * i);
        unsigned fmt = rd16(s);
        if (fmt == 4) {
            unsigned seg = rd16(s + 6) / 2;
            for (unsigned k = 0; k < seg; k++)
                for (unsigned cp = rd16(s + 16 + 2 * seg + 2 * k), e = rd16(s + 14 + 2 * k); cp <= e && cp < 0xffff; cp++) cp_set(t->has, cp);
        } else if (fmt == 12) {
            for (unsigned g = 0, ng = rd32(s + 12); g < ng; g++)
                for (unsigned cp = rd32(s + 16 + 12 * g), e = rd32(s + 20 + 12 * g); cp <= e && cp < 0x10000; cp++) cp_set(t->has, cp);
        }
    }
    int asc = (short)rd16(h + 4), desc = (short)rd16(h + 6);
    t->base = asc - desc > 0 ? (int)((float)asc * FONT_PX / (asc - desc)) : FONT_PX;
    return 1;
}
static void cps_walk(cJSON *j, unsigned char *want) {
    for (; j; j = j->next) {
        if (cJSON_IsString(j))
            for (const char *s = j->valuestring; *s;) {
                int n = 1, cp = GetCodepointNext(s, &n);
                cp_set(want, (unsigned)cp);
                s += n > 0 ? n : 1;
            }
        if (j->child) cps_walk(j->child, want);
    }
}
static GlyphInfo *glyphs_of(const TTF *t, int *cps, int n) {
#if RAYLIB_VERSION_MAJOR >= 6
    int got = 0;
    return LoadFontData(t->d, t->n, FONT_PX, cps, n, FONT_DEFAULT, &got);
#else
    return LoadFontData(t->d, t->n, FONT_PX, cps, n, FONT_DEFAULT);
#endif
}

#ifdef __vita__
#include <psp2/pvf.h>
static void *pvf_alloc(void *u, unsigned sz) { (void)u; return malloc(sz); }
static void *pvf_realloc(void *u, void *p, unsigned sz) { (void)u; return realloc(p, sz); }
static void pvf_free(void *u, void *p) { (void)u; free(p); }
static ScePvfFontId pvf_font(int lang) {  // il font della console per la lingua (giapponese, coreano o cinese), tarato sull'altezza di 日
    static ScePvfLibId lib;
    static ScePvfFontId open[6];
    ScePvfError e = 0;
    if (!lib) {
        ScePvfInitRec r = { NULL, 4, NULL, NULL, pvf_alloc, pvf_realloc, pvf_free };
        if (!(lib = scePvfNewLib(&r, &e))) return NULL;
        scePvfSetEM(lib, 72.0f / (10.125f * 128.0f));
        scePvfSetResolution(lib, 128.0f, 128.0f);
    }
    if (open[lang]) return open[lang];
    ScePvfFontStyleInfo st;
    memset(&st, 0, sizeof(st));
    st.languageCode = lang, st.familyCode = SCE_PVF_FAMILY_SANSERIF, st.style = SCE_PVF_STYLE_REGULAR;
    ScePvfFontIndex i = scePvfFindOptimumFont(lib, &st, &e);
    ScePvfFontId f = e >= 0 && i >= 0 ? scePvfOpen(lib, i, 1, &e) : NULL;
    if (!f) f = scePvfOpenDefaultJapaneseFontOnSharedMemory(lib, &e);
    if (!f) return NULL;
    // ScePvfCharInfo.bitmap* resta a 0 su questa libreria: misure e posizioni dalle glyphMetrics (26.6)
    ScePvfCharInfo ci;
    scePvfSetCharSize(f, FONT_PX, FONT_PX);
    if (scePvfGetCharInfo(f, 0x65e5, &ci) >= 0 && ci.glyphMetrics.height64 > 0) {  // 日 alto ~0.8 em, come nel browser a 32 px
        float k = FONT_PX * 0.8f * FONT_PX / (ci.glyphMetrics.height64 / 64.0f);
        scePvfSetCharSize(f, k, k);
    }
    return open[lang] = f;
}
static int pvf_glyph(ScePvfFontId f, int cp, int base, GlyphInfo *g) {
    ScePvfCharInfo ci;
    if (!scePvfIsElement(f, (ScePvfCharCode)cp) || scePvfGetCharInfo(f, (ScePvfCharCode)cp, &ci) < 0) return 0;
    const ScePvfIGlyphMetricsInfo *m = &ci.glyphMetrics;
    int left = (int)floorf(m->horizontalBearingX64 / 64.0f), top = (int)ceilf(m->horizontalBearingY64 / 64.0f);
    int w = (int)ceilf((m->horizontalBearingX64 - left * 64 + (int)m->width64) / 64.0f) + 4, h = (int)ceilf((top * 64 - m->horizontalBearingY64 + (int)m->height64) / 64.0f) + 4;
    unsigned char *px = RL_CALLOC((size_t)w * h, 1);
    // la penna (origine sulla linea di base) a (2 - left, 2 + top): il glifo comincia a 2 px dal bordo
    ScePvfUserImageBufferRec b = { SCE_PVF_USERIMAGE_DIRECT8, (2 - left) * 64, (2 + top) * 64, { (ScePvfU16)w, (ScePvfU16)h }, (ScePvfU16)w, 0, px };
    scePvfGetCharGlyphImage(f, (ScePvfCharCode)cp, &b);
    *g = (GlyphInfo){ cp, left - 2, base - top - 2, (int)lroundf(m->horizontalAdvance64 / 64.0f), { px, w, h, 1, PIXELFORMAT_UNCOMPRESSED_GRAYSCALE } };
    return 1;
}
#endif

// Cache del font finito (atlante + glifi) per lingua: costruirlo costa secondi con le lingue CJK (3238 glifi PVF per il cinese).
// Chiave: i caratteri richiesti, la lingua, FONT_PX e la versione del formato; se cambia qualcosa il file si rigenera.
#define FONT_CACHE_VER 2  // 2: atlante con gli zeri compressi (byte 0 + quanti, fino a 255)
typedef struct { unsigned magic, key; int count, w, h, packed; } FontCacheHead;
static unsigned fnv1a(const void *p, size_t n, unsigned h) {
    for (size_t i = 0; i < n; i++) h = (h ^ ((const unsigned char *)p)[i]) * 16777619u;
    return h;
}
static double fc_t[4];  // misura: lettura, conversione, texture
static int font_cache_load(const char *path, unsigned key, Font *f) {
    fc_t[0] = GetTime();
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    setvbuf(fp, NULL, _IOFBF, 1 << 20);  // letture grosse: con il buffer piccolo di newlib la memory card dava ~3 MB/s
    FontCacheHead hd;
    int ok = fread(&hd, sizeof(hd), 1, fp) == 1 && hd.magic == 0x46435653 && hd.key == key && hd.count > 0 && hd.w > 0 && hd.h > 0;
    GlyphInfo *g = ok ? RL_CALLOC((size_t)hd.count, sizeof(GlyphInfo)) : NULL;
    Rectangle *r = ok ? RL_CALLOC((size_t)hd.count, sizeof(Rectangle)) : NULL;
    unsigned char *a = ok ? malloc((size_t)hd.w * hd.h) : NULL;
    for (int i = 0; ok && i < hd.count; i++) {
        int m[4];
        ok = fread(m, sizeof(m), 1, fp) == 1 && fread(&r[i], sizeof(Rectangle), 1, fp) == 1;
        g[i] = (GlyphInfo){ m[0], m[1], m[2], m[3], { 0 } };
    }
    unsigned char *pk = ok ? malloc((size_t)hd.packed) : NULL;
    ok = ok && hd.packed > 0 && fread(pk, 1, (size_t)hd.packed, fp) == (size_t)hd.packed;
    fclose(fp);
    size_t o = 0, n = (size_t)hd.w * hd.h;
    for (int i = 0; ok && i < hd.packed; i++) {  // gli zeri compressi
        if (pk[i]) { ok = o < n; if (ok) a[o++] = pk[i]; continue; }
        int run = i + 1 < hd.packed ? pk[++i] : 0;
        ok = o + run <= n;
        if (ok) memset(a + o, 0, run), o += run;
    }
    ok = ok && o == n;
    free(pk);
    fc_t[1] = GetTime();
    if (!ok) { RL_FREE(g), RL_FREE(r), free(a); return 0; }
    unsigned char *ga = RL_MALLOC((size_t)hd.w * hd.h * 2);  // GRAY_ALPHA come GenImageFontAtlas: bianco con l'alpha del glifo
    for (int i = 0; i < hd.w * hd.h; i++) ga[2 * i] = 255, ga[2 * i + 1] = a[i];
    free(a);
    fc_t[2] = GetTime();
    Image im = { ga, hd.w, hd.h, 1, PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA };
    *f = (Font){ FONT_PX, hd.count, 2, LoadTextureFromImage(im), r, g };
    UnloadImage(im);
    fc_t[3] = GetTime();
    return f->texture.id != 0;
}
static void font_cache_save(const char *path, unsigned key, const Font *f, const Image *atlas) {
    if (atlas->format != PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA) return;
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    size_t n = (size_t)atlas->width * atlas->height, k = 0;
    const unsigned char *ga = atlas->data;
    unsigned char *pk = malloc(n * 2 + 2);
    for (size_t i = 0; i < n;) {
        unsigned char v = ga[2 * i + 1];
        if (v) { pk[k++] = v, i++; continue; }
        int run = 0;
        while (i < n && run < 255 && !ga[2 * i + 1]) run++, i++;
        pk[k++] = 0, pk[k++] = (unsigned char)run;
    }
    FontCacheHead hd = { 0x46435653, key, f->glyphCount, atlas->width, atlas->height, (int)k };
    fwrite(&hd, sizeof(hd), 1, fp);
    for (int i = 0; i < f->glyphCount; i++) {
        int m[4] = { f->glyphs[i].value, f->glyphs[i].offsetX, f->glyphs[i].offsetY, f->glyphs[i].advanceX };
        fwrite(m, sizeof(m), 1, fp), fwrite(&f->recs[i], sizeof(Rectangle), 1, fp);
    }
    fwrite(pk, 1, k, fp);
    free(pk);
    fclose(fp);
}
static void font_log(double t0, int cached) {  // <dati>/font.txt: lingua, glifi, millisecondi, cache
    FILE *fp = fopen(vs_path("font.txt"), "a");
    if (!fp) return;
    fprintf(fp, "%s %d glifi %.0f ms", lang_code, vs_font.glyphCount, 1000 * (GetTime() - t0));
    if (cached) fprintf(fp, " (cache: lettura %.0f, conversione %.0f, texture %.0f)", 1000 * (fc_t[1] - fc_t[0]), 1000 * (fc_t[2] - fc_t[1]), 1000 * (fc_t[3] - fc_t[2]));
    fputc('\n', fp), fclose(fp);
}
static void font_use(Font f) {
    SetTextureFilter(f.texture, TEXTURE_FILTER_BILINEAR);
    if (vs_font.texture.id && vs_font.texture.id != GetFontDefault().texture.id) UnloadFont(vs_font);
    vs_font = f;
    glyph_index_build();
}

void vs_load_font(void) {
    double t0 = GetTime();
    static const char *const FILES[] = { "achievementLang", "arcanaLang", "characterLang", "enemiesLang", "itemLang", "powerUpLang", "secretLang", "stageLang", "weaponLang" };
    unsigned char *want = calloc(0x2000, 1), *taken = calloc(0x2000, 1);
    for (unsigned c = 32; c < 0x250; c++) cp_set(want, c);
    for (unsigned c = 0x400; c < 0x460; c++) cp_set(want, c);
    if (!lang_all) lang_all = read_json("lang/lang.json");
    cJSON *cur = cJSON_GetObjectItem(lang_all, lang_code);
    if (cur) cps_walk(cur->child, want);
    for (size_t i = 0; i < sizeof(FILES) / sizeof(*FILES); i++) {
        cJSON *l = cJSON_GetObjectItem(lang_file(FILES[i]), lang_code);
        if (l) cps_walk(l->child, want);
    }
    for (int i = 0; Lang_codeAt(i); i++) cps_walk(cJSON_GetObjectItem(cJSON_GetObjectItem(lang_all, Lang_codeAt(i)), "native"), want);
    static cJSON *credits;  // data/credits.json: i nomi dei traduttori (hangul, kanji, cuori) in qualunque lingua
    if (!credits) credits = read_json("data/credits.json");
    if (credits) cps_walk(credits->child, want);
    char cpath[600];
    snprintf(cpath, sizeof(cpath), "%s", vs_path(TextFormat("font_%s.cache", lang_code)));
#ifdef __vita__
    const unsigned plat = 0x56495441;  // i glifi CJK vengono dai font della console: mai la cache fatta sul desktop
#else
    const unsigned plat = 0;
#endif
    unsigned key = fnv1a(want, 0x2000, fnv1a(lang_code, strlen(lang_code), 2166136261u)) ^ (FONT_PX * 31 + FONT_CACHE_VER) ^ plat;
    Font cached;
    if (font_cache_load(cpath, key, &cached)) { font_use(cached); free(want), free(taken); font_log(t0, 1); return; }
    int cjk = !strcmp(lang_code, "ja") ? 1 : !strcmp(lang_code, "ko") ? 3 : !strcmp(lang_code, "zh-CN") ? 4 : 0;  // SCE_PVF_LANGUAGE_J/K/C
    static TTF any, ko, sys;  // aperti una volta: rileggerli a ogni cambio di lingua costava (2 MB dalla memory card)
    static int opened;
    if (!opened) {
        opened = 1;
        ttf_open(&any, vs_path("fonts/ANY.ttf"));
        ttf_open(&ko, vs_path("fonts/KO.ttf"));
#ifndef __vita__
        ttf_open(&sys, "/System/Library/Fonts/Supplemental/Arial Unicode.ttf");
#endif
    }
    TTF *order[3] = { cjk ? &ko : &any, cjk ? &any : &ko, &sys };
    int base = order[0]->d ? order[0]->base : order[1]->d ? order[1]->base : FONT_PX * 3 / 4;
    int cap = 0, total = 0, *list = malloc(sizeof(int) * 0x10000);
    for (unsigned c = 0; c < 0x10000; c++) cap += cp_has(want, c);
    GlyphInfo *all = RL_CALLOC((size_t)cap + 1, sizeof(GlyphInfo));
    for (int s = 0; s < 3; s++) {
        TTF *t = order[s];
        if (!t->d) continue;
        int n = 0;
        for (unsigned c = 32; c < 0x10000; c++) if (cp_has(want, c) && !cp_has(taken, c) && cp_has(t->has, c)) list[n++] = (int)c, cp_set(taken, c);
        GlyphInfo *g = n ? glyphs_of(t, list, n) : NULL;
        for (int i = 0; g && i < n; i++) g[i].offsetY += base - t->base, all[total++] = g[i];
        if (g) RL_FREE(g);  // le immagini passano ad all
    }
#ifdef __vita__
    ScePvfFontId pf = NULL;
    for (unsigned c = 0x2e80; c < 0x10000; c++) {  // il resto (ideogrammi e punteggiatura CJK) dal font della console
        if (!cp_has(want, c) || cp_has(taken, c)) continue;
        if (!pf && !(pf = pvf_font(cjk ? cjk : 1))) break;
        if (pvf_glyph(pf, (int)c, base, &all[total])) { total++; continue; }
        static const int other[3] = { 4, 1, 3 };  // manca nel font della lingua (简 nel giapponese): gli altri font della console
        for (int k = 0; k < 3; k++) {
            ScePvfFontId of = other[k] != (cjk ? cjk : 1) ? pvf_font(other[k]) : NULL;
            if (of && of != pf && pvf_glyph(of, (int)c, base, &all[total])) { total++; break; }
        }
    }
#endif
    Font f = { 0 };
    f.baseSize = FONT_PX, f.glyphCount = total, f.glyphPadding = 2, f.glyphs = all;
    Image atlas = GenImageFontAtlas(all, &f.recs, total, FONT_PX, 2, 1);
    f.texture = LoadTextureFromImage(atlas);
    if (f.texture.id) font_cache_save(cpath, key, &f, &atlas);
    UnloadImage(atlas);
    if (f.texture.id) font_use(f);
    else if (!vs_font.texture.id) vs_font = GetFontDefault();
    font_log(t0, 0);
    free(list), free(want), free(taken);
}

// --- Video ---------------------------------------------------------------------
struct VsVideo {
    Texture2D tex;
    int done;
#ifdef __vita__
    SceAvPlayerHandle player;
    int slot, started;
    float waited;
#else
    FILE *pipe;
    unsigned char *buf;
    float t;
    int frame;
#endif
};
#ifdef __vita__
// Fotogrammi NV12 (allineamento 256 KiB, come il lettore di VitaShell); la texture GXM in formato
// YVU420P2_CSC1 li converte in RGB da sola. Le texture GL restano vive (pool di 4): i loro dati li ha liberati vglFree.
static void av_log(const char *fmt, ...) {  // diagnosi: <dati>/video.log
    FILE *f = fopen(vs_path("video.log"), "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt), vfprintf(f, fmt, ap), va_end(ap);
    fputc('\n', f), fclose(f);
}
static void *av_alloc(void *p, uint32_t align, uint32_t size) { (void)p; void *r = memalign(align, size); if (!r) av_log("alloc %u align %u FALLITA", size, align); return r; }
static void av_free(void *p, void *ptr) { (void)p; free(ptr); }
// Fotogrammi e buffer del decoder: memoria vista dal decodificatore hardware, cioe' VRAM o RAM fisicamente contigua dai pool di
// vitaGL (gia' mappati sulla GPU), allineata a mano. av_blocks ricorda il puntatore vero per vglFree.
// Il decoder pero' vuole la BASE di un memblock tutto suo, allineato dal kernel (provato sulla Vita con avtest: un
// pezzo di un blocco piu' grande -> solo l'evento STOP). Quindi prima un memblock CDRAM, poi fisicamente contiguo: c'e'
// memoria libera solo se avres.req l'ha tenuta fuori da vitaGL (main.c). uid >= 0: blocco del kernel.
// I memblock non si liberano mai, si riusano (used = 0): vitaGL manda i disegni alla GPU a fine scena, e smappare
// i fotogrammi di un video chiuso a meta' fotogramma faceva GPUCRASH sulla Vita. I tre video hanno buffer uguali.
static struct { void *real, *aligned; SceUID uid; uint32_t len; int used; } av_blocks[32];
static void *av_gpu_alloc(void *p, uint32_t align, uint32_t size) {
    (void)p;
    if (align < 0x40000) align = 0x40000;
    for (int k = 0; k < 32; k++)
        if (av_blocks[k].real && av_blocks[k].uid >= 0 && !av_blocks[k].used && av_blocks[k].len >= size && !((uintptr_t)av_blocks[k].real & (align - 1))) {
            av_blocks[k].used = 1;
            return av_blocks[k].real;
        }
    static const SceKernelMemBlockType K[2] = { SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW };
    for (int t = 0; t < 2; t++) {
        uint32_t a = t && align < 0x100000 ? 0x100000 : align, len = (size + a - 1) & ~(a - 1);
        SceKernelAllocMemBlockOpt opt;
        memset(&opt, 0, sizeof(opt));
        opt.size = sizeof(opt), opt.attr = 4, opt.alignment = a;  // 4: alignment valido
        SceUID uid = sceKernelAllocMemBlock("avplayer", K[t], len, &opt);
        if (uid < 0) continue;
        void *base;
        sceKernelGetMemBlockBase(uid, &base);
        if (sceGxmMapMemory(base, len, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE) < 0) { sceKernelFreeMemBlock(uid); continue; }
        for (int k = 0; k < 32; k++) if (!av_blocks[k].real) { av_blocks[k].real = av_blocks[k].aligned = base, av_blocks[k].uid = uid, av_blocks[k].len = len, av_blocks[k].used = 1; break; }
        av_log("gpu_alloc %u align %u -> %p memblock %d", size, align, base, t);
        return base;
    }
    static const vglMemType T[3] = { VGL_MEM_VRAM, VGL_MEM_SLOW, VGL_MEM_BUDGET };
    for (int t = 0; t < 3; t++) {
        if (vglMemFree(T[t]) < size + align) continue;
        void *real = vglAlloc(size + align, T[t]);
        if (!real) continue;
        void *al = (void *)(((uintptr_t)real + align - 1) & ~(uintptr_t)(align - 1));
        for (int k = 0; k < 32; k++) if (!av_blocks[k].real) { av_blocks[k].real = real, av_blocks[k].aligned = al, av_blocks[k].uid = -1; break; }
        av_log("gpu_alloc %u align %u -> %p tipo %d", size, align, al, t);
        return al;
    }
    av_log("gpu_alloc %u FALLITA (vram %u slow %u budget %u)", size, (unsigned)vglMemFree(VGL_MEM_VRAM), (unsigned)vglMemFree(VGL_MEM_SLOW), (unsigned)vglMemFree(VGL_MEM_BUDGET));
    return NULL;
}
static void av_gpu_free(void *p, void *ptr) {
    (void)p;
    for (int k = 0; k < 32; k++) if (av_blocks[k].aligned == ptr && av_blocks[k].real) {
        if (av_blocks[k].uid >= 0) av_blocks[k].used = 0;
        else vglLazyFree(av_blocks[k].real), av_blocks[k].real = NULL;  // liberato quando la GPU ha finito di usarlo
        return;
    }
}
static struct { GLuint id; SceGxmTexture *gxm; int used; } av_slots[4];
static void av_event(void *p, int32_t id, int32_t src, void *data) { (void)p, (void)data; av_log("evento %d sorgente %d", id, src); }
VsVideo *vs_video_open(const char *rel, int w, int h) {
    static int loaded;
    if (!loaded) { int r = sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER); loaded = r >= 0 ? 1 : -1; av_log("sysmodule %08x", r); }
    int s = 0;
    while (s < 4 && av_slots[s].used) s++;
    if (loaded < 0 || s == 4) return NULL;
    if (!av_slots[s].id) {
        glGenTextures(1, &av_slots[s].id);
        glBindTexture(GL_TEXTURE_2D, av_slots[s].id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        av_slots[s].gxm = vglGetGxmTexture(GL_TEXTURE_2D);
        vglFree(vglGetTexDataPointer(GL_TEXTURE_2D));
    }
    SceAvPlayerInitData d;
    memset(&d, 0, sizeof(d));
    d.memoryReplacement.allocate = av_alloc, d.memoryReplacement.deallocate = av_free;
    d.memoryReplacement.allocateTexture = av_gpu_alloc, d.memoryReplacement.deallocateTexture = av_gpu_free;
    d.eventReplacement.eventCallback = av_event;
    d.basePriority = 0xa0, d.numOutputVideoFrameBuffers = 2, d.autoStart = 1;
    SceAvPlayerHandle pl = sceAvPlayerInit(&d);
    av_log("init %s slot %d -> %08x gxm %p", rel, s, pl, (void *)av_slots[s].gxm);
    if (!pl || ((unsigned)pl & 0xffff0000u) == 0x806a0000u) return NULL;  // l'handle e' un indirizzo (sopra 0x80000000): errore solo 0x806Axxxx
    int as = sceAvPlayerAddSource(pl, vs_path(rel));
    av_log("addsource %08x", as);
    if (as < 0) { sceAvPlayerClose(pl); return NULL; }
    VsVideo *v = calloc(1, sizeof(VsVideo));
    v->player = pl, v->slot = s, av_slots[s].used = 1;
    v->tex = (Texture2D){ av_slots[s].id, w, h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    return v;
}
int vs_video_update(VsVideo *v, float ms) {
    if (!v || v->done) return 0;
    if (!sceAvPlayerIsActive(v->player)) {  // parte in modo asincrono: finito solo dopo essere partito, o se non parte in 3 s
        v->waited += ms;
        if (v->started || v->waited > 3000) { av_log("fine (partito %d, attesa %.0f ms)", v->started, v->waited); return !(v->done = 1); }
        return 1;
    }
    v->started = 1;
    SceAvPlayerFrameInfo f;
    if (sceAvPlayerGetVideoData(v->player, &f)) {
        static int nf;
        if (nf++ % 60 == 0) av_log("frame %d data %p %ux%u ts %llu", nf, (void *)f.pData, f.details.video.width, f.details.video.height, (unsigned long long)f.timeStamp);
        SceGxmTexture *t = av_slots[v->slot].gxm;
        sceGxmTextureInitLinear(t, f.pData, SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1, f.details.video.width, f.details.video.height, 0);
        sceGxmTextureSetMinFilter(t, SCE_GXM_TEXTURE_FILTER_LINEAR), sceGxmTextureSetMagFilter(t, SCE_GXM_TEXTURE_FILTER_LINEAR);
    }
    return 1;
}
void vs_video_close(VsVideo *v) {
    if (!v) return;
    sceAvPlayerStop(v->player);
    sceAvPlayerClose(v->player);
    av_slots[v->slot].used = 0;
    free(v);
}
#else
VsVideo *vs_video_open(const char *rel, int w, int h) {  // semplificazione: anteprima sul Mac, serve ffmpeg nel PATH
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "ffmpeg -v quiet -i '%s' -f rawvideo -pix_fmt rgba -", vs_path(rel));
    FILE *p = popen(cmd, "r");
    if (!p) return NULL;
    VsVideo *v = calloc(1, sizeof(VsVideo));
    v->pipe = p, v->buf = calloc((size_t)w * h, 4);
    Image im = { v->buf, w, h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    v->tex = LoadTextureFromImage(im);
    SetTextureFilter(v->tex, TEXTURE_FILTER_BILINEAR);
    return v;
}
int vs_video_update(VsVideo *v, float ms) {  // i fotogrammi a 30 fps sul tempo trascorso
    if (!v || v->done) return 0;
    v->t += ms;
    size_t n = (size_t)v->tex.width * v->tex.height * 4;
    int want = (int)(v->t * 30 / 1000), got = 0;
    while (v->frame < want) {
        if (fread(v->buf, 1, n, v->pipe) != n) return !(v->done = 1);
        v->frame++, got = 1;
    }
    if (got) UpdateTexture(v->tex, v->buf);
    return 1;
}
void vs_video_close(VsVideo *v) {
    if (!v) return;
    pclose(v->pipe);
    UnloadTexture(v->tex);
    free(v->buf), free(v);
}
#endif
Texture2D vs_video_texture(const VsVideo *v) { return v ? v->tex : (Texture2D){ 0 }; }
