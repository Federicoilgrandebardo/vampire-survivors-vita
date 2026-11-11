// Vampire Survivors per PS Vita: riscrittura nativa (C + raylib) della build Phaser.
// Qui: avvio e loop principale. Menu (IntroScene e seguenti) e partita stanno in vs_game.c.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "trophies.h"
#include "vs.h"
#include "rlgl.h"
#include "audio.h"
#include "net.h"
#include "phaser.h"

static Scene scene;
static char **main_argv;

void scene_set(Scene s) { scene = s; }

#ifdef __vita__
#include <psp2/appmgr.h>
void vs_reload(void) { sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL); }
// vglInitExtended (via SDL2) prende tutta la CDRAM e la RAM fisicamente contigua: i video (sceAvPlayer) vogliono memblock
// tutti loro (runtime.c). Si tiene memoria fuori da vitaGL riservandola prima di InitWindow e liberandola subito dopo.
// Predefiniti: 16 MiB di CDRAM (provato sulla Vita: i buffer di un video 640x336 sono ~10 MiB, un decoder alla volta);
// <dati>/avres.req = "MiB_phycont MiB_cdram" per tararla.
#include <psp2/kernel/sysmem.h>
static SceUID av_res[2] = { -1, -1 };
static void av_reserve(void) {
    int mb[2] = { 0, 16 };
    FILE *f = fopen("ux0:data/VampireSurvivors/avres.req", "r");
    if (f) fscanf(f, "%d %d", &mb[0], &mb[1]), fclose(f);
    static const SceKernelMemBlockType T[2] = { SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW };
    for (int i = 0; i < 2; i++) if (mb[i] > 0) av_res[i] = sceKernelAllocMemBlock("avres", T[i], mb[i] << 20, NULL);
}
static void av_release(void) { for (int i = 0; i < 2; i++) if (av_res[i] >= 0) sceKernelFreeMemBlock(av_res[i]); }
#else
static void av_reserve(void) {}
static void av_release(void) {}
#include <unistd.h>
void vs_reload(void) {  // stesso eseguibile; senza gli agganci di navigazione, se no la prova si ripeterebbe all'infinito
    unsetenv("VS_MENU"), unsetenv("VS_AUTOPICK");
    execv(main_argv[0], main_argv);
}
#endif

#ifdef __vita__
#include <psp2/ctrl.h>
const char *menu_dbg(void);
static void pad_log(void) {  // sviluppo: VS_PADLOG=1 in env.req -> <dati>/pad.txt ogni 30 frame (raylib/SDL e sceCtrl grezzo)
    static int n;
    if (!getenv("VS_PADLOG") || ++n % 30) return;
    SceCtrlData c = { 0 };
    sceCtrlPeekBufferPositive(0, &c, 1);
    unsigned rl = 0;
    for (int b = 1; b <= 17; b++) rl |= (unsigned)IsGamepadButtonDown(0, b) << b;
    FILE *f = fopen(vs_path("pad.txt"), "a");
    if (!f) return;
    fprintf(f, "f=%d avail=%d name=%s rl=%05x ax=%.2f,%.2f sce=%08x lx=%d ly=%d mouse=%d,%d down=%d touch=%d %s\n", n, IsGamepadAvailable(0), IsGamepadAvailable(0) ? GetGamepadName(0) : "-",
            rl, GetGamepadAxisMovement(0, 0), GetGamepadAxisMovement(0, 1), c.buttons, c.lx, c.ly, GetMouseX(), GetMouseY(), IsMouseButtonDown(0), GetTouchPointCount(), menu_dbg());
    fclose(f);
}
#else
static void pad_log(void) {}
#endif

#ifdef __vita__
// semplificazione: solo per il linker. In raudio.o ma_dlopen/ma_dlsym/ma_dlclose esistono ma nessuno li chiama
// (objdump: zero chiamanti); il backend del porting è ma_context_init__vita, su sceAudioOut, e non li usa.
void *dlopen(const char *f, int m) { (void)f; (void)m; return 0; }
void *dlsym(void *h, const char *s) { (void)h; (void)s; return 0; }
int dlclose(void *h) { (void)h; return 0; }
#endif


// Sonda del lampo nero (VS_PROBE=1, sviluppo): ogni frame due righe di pixel in fondo allo schermo dopo la presentazione;
// tratti di nero pieno >= 60 px nel campo di gioco -> foto PNG di quel frame. SELECT = segnaposto di chi gioca ("visto ora").
// Log per frame (tasti, stick, durata, costo della lettura, nero trovato) e contatori ogni secondo in <dati>/probe/log.txt.
// Niente miniature periodiche: sulla Vita leggere lo schermo intero costa ~1,5 s.
#ifdef __vita__
#include <vitaGL.h>
#include <psp2/gxm.h>
extern SceGxmNotification query_fence;  // vitaGL: value = scene mandate alla GPU, *address = l'ultima finita
extern uint32_t vsync_interval;         // vitaGL: 1 = la callback di display aspetta il vblank
#else
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#endif
int vs_finish_mode(void) {
    static int m = -1;
    // Vita: di serie 3 (rt + swap). Senza, lo schermo a volte leggeva la vista di gioco (rt) o il frame (swap) prima che la
    // GPU li finisse: lampo nero in basso (le ultime tessere, ancora pulite a nero). Provato da Federico 2026-10-25.
    // Costo misurato con Pasqualina dopo la raylib senza MSAA: rt ~0,5 ms, swap ~1,5 ms a frame.
    // 2026-11-05: spenta anche sulla Vita. Sei armi evolute e 300 nemici: da ~39 a ~56 fps, un lampo nero in 3 minuti
    // (prova di Federico). L'idea e' di DrakonPL. VS_FINISH=swap rimette l'attesa.
    // Bug 4 del test finale: profilo nelle Opzioni (vs_quality_mode): Prestazioni = 0 (di serie), Qualita' = 3 (niente lampi).
    // 2026-11-11: i lampi erano il vsync spento (vedi main): con l'attesa del vblank non ne servono altre, 0 in tutti e due
    // i profili. VS_FINISH resta per le prove.
    const char *e = getenv("VS_FINISH");
    if (!e) return 0;
    if (m < 0) m = !strcmp(e, "rt") ? 1 : !strcmp(e, "swap") ? 2 : !strcmp(e, "off") ? 0 : !strcmp(e, "ab") ? 4 : 3;
    if (m == 4) return ((int)(GetTime() / 10) & 1) ? 3 : 1;  // VS_FINISH=ab: rt e both a turno ogni 10 s (misura in una partita)
    return m;
}
void vs_gl_finish(void) { glFinish(); }
const char *game_prof(void);
static int probe_run(const unsigned char *row, int *at) {  // tratto nero piu' lungo tra x 50 e 910
    int best = 0, run = 0;
    for (int x = 50; x < 910; x++) {
        const unsigned char *p = row + 4 * x;
        if (p[0] < 4 && p[1] < 4 && p[2] < 4) { if (++run > best) best = run, *at = x - run + 1; }
        else run = 0;
    }
    return best;
}
static void probe_frame(int scene_game, float dt) {
    static int on = -1, frame, nshots, sec;
    static double lastShot = -10, lastFlush;
    static unsigned lastPad;
    static char *buf;
    static size_t len;
    if (on < 0) {
        on = getenv("VS_PROBE") != NULL;
        if (on) MakeDirectory(vs_path("probe")), buf = malloc(1 << 17), lastFlush = GetTime();
    }
    if (!on) return;
    frame++;
    static unsigned char band[31][SCREEN_W * 4];  // y 500..530 in una lettura, ogni 3 frame (ogni lettura aspetta la GPU, ~7 ms)
    int run[2] = { -1, -1 }, at[2] = { 0, 0 };
    double r0 = GetTime();
    if (frame % 3 == 0) {
        glReadPixels(0, SCREEN_H - 1 - 530, SCREEN_W, 31, GL_RGBA, GL_UNSIGNED_BYTE, band);  // riga 0 = y 530 (GL dal basso)
        run[0] = 0, run[1] = 0;
        for (int r = 0; r < 31; r++) {
            int a = 0, n = probe_run(band[r], &a);
            int k = r < 15 ? 1 : 0;  // k 1 = meta' bassa (y 516..530), 0 = alta (500..515)
            if (n > run[k]) run[k] = n, at[k] = a;
        }
    }
    double rd = GetTime() - r0;
    unsigned pad = 0;
    int lx = 128, ly = 128;
#ifdef __vita__
    SceCtrlData c = { 0 };
    sceCtrlPeekBufferPositive(0, &c, 1);
    pad = c.buttons, lx = c.lx, ly = c.ly;
#endif
    int mark = (pad & 1) && !(lastPad & 1);  // SCE_CTRL_SELECT appena premuto
    lastPad = pad;
    double now = GetTime();
    int black = scene_game && frame > 60 && (run[0] >= 60 || run[1] >= 60);  // un pipistrello nero pieno e' largo ~30 px
    const size_t cap = (1 << 17) - 1024;
    if (len < cap)
        len += snprintf(buf + len, cap + 1024 - len, "%d %.3f dt=%.1f rd=%.1f pad=%05x lx=%d ly=%d nero500-515=%d@%d nero516-530=%d@%d%s%s\n", frame, now, dt * 1000, rd * 1000, pad, lx, ly,
                        run[0], at[0], run[1], at[1], black ? " <<<" : "", mark ? " MARK" : "");
    if (mark && len < cap) len += snprintf(buf + len, cap + 1024 - len, "MARK %s\n", scene_game ? game_prof() : "menu");
    if (black && now - lastShot > 2 && nshots < 20) {
        nshots++;
        Image im = LoadImageFromScreen();
        ExportImage(im, vs_path(TextFormat("probe/nero_%02d_f%d.png", nshots, frame)));
        UnloadImage(im);
        if (len < cap) len += snprintf(buf + len, cap + 1024 - len, "FOTO nero_%02d_f%d %s\n", nshots, frame, scene_game ? game_prof() : "menu");
        lastShot = GetTime();
    }
    if (now - lastFlush >= 1) {
        if (len < cap) len += snprintf(buf + len, cap + 1024 - len, "SEC %d %s\n", sec++, scene_game ? game_prof() : "menu");
        FILE *f = fopen(vs_path("probe/log.txt"), "a");
        if (f) fwrite(buf, 1, len, f), fclose(f);
        len = 0;
        lastFlush = GetTime();  // dopo la scrittura: niente rincorsa se il frame era lento
    }
}

int det_on;
unsigned det_frame, det_calls;
static unsigned det_s;
int det_rand(void) {
    if (!det_on) return rand();
    det_calls++;
    det_s ^= det_s << 13, det_s ^= det_s >> 17, det_s ^= det_s << 5;
    return (int)(det_s >> 1);
}
unsigned physics_hash(void);
void det_game_hash(unsigned *enemies, unsigned *player);
void det_begin(unsigned seed) { det_on = 1, det_s = seed | 1, det_frame = det_calls = 0; }
void det_end(void) { det_on = 0; }
static void det_log(void) {
    static FILE *f;
    if (!f && getenv("VS_DET")) f = fopen(vs_path("det.txt"), "w");
    unsigned e, p;
    det_game_hash(&e, &p);
    if (f) {
        fprintf(f, "%u %u %08x %08x %08x\n", det_frame, det_calls, physics_hash(), e, p);
        if (det_frame % 300 == 0) fflush(f);
    }
    if (net_on) net_frame_hash(det_frame, physics_hash() ^ e * 31 ^ p * 131 ^ det_calls);
    det_frame++;
}
void game_net_submit(void);
void game_net_end(void);
int main(int argc, char **argv) {
    (void)argc;
    main_argv = argv;
    av_reserve();
    InitWindow(SCREEN_W, SCREEN_H, "Vampire Survivors");
    av_release();
    srand((unsigned)time(NULL));  // Math.random: un seme diverso a ogni accensione (menu compreso)
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);  // ESC = indietro, non chiudere
    vs_init_paths();
    // Sulla Vita niente ambiente: <dati>/env.req con righe CHIAVE=valore (gli stessi agganci VS_* del desktop)
    char *env = LoadFileText(vs_path("env.req"));
    for (char *l = env ? strtok(env, "\r\n") : NULL; l; l = strtok(NULL, "\r\n")) {
        char *eq = strchr(l, '=');
        if (eq) *eq = 0, setenv(l, eq + 1, 1);
    }
    if (env) UnloadFileText(env);
#ifdef __vita__
    // raylib senza FLAG_VSYNC_HINT toglie a vitaGL l'attesa del vblank nella callback di display: GXM da' per libero il
    // buffer che e' ancora a schermo e un frame successivo lo ripulisce a nero mentre lo schermo lo legge (lampo nero in
    // basso). Con l'attesa il buffer torna libero solo quando lo schermo e' passato al nuovo. VS_VSYNC=0: senza (controllo).
    vglWaitVblankStart(!getenv("VS_VSYNC") || atoi(getenv("VS_VSYNC")));
#endif
    if (getenv("VS_DET")) det_on = 1, det_s = (unsigned)atoi(getenv("VS_DET")) | 1;
    if (getenv("VS_NET")) {  // sviluppo: VS_NET=host | <indirizzo>
        unsigned seed = getenv("VS_DET") ? (unsigned)atoi(getenv("VS_DET")) : 11;
        const char *n = getenv("VS_NET");
        if (net_open(strcmp(n, "host") ? n : NULL, &seed)) det_begin(seed);
    }
    if (!getenv("VS_MUTE")) InitAudioDevice(), au_start();  // VS_MUTE: prove automatiche senza audio
    if (!data_get("characters")) {
        // Senza dati non c'e' gioco: si spiega come prepararli invece di chiudere in silenzio.
        while (!WindowShouldClose()) {
            BeginDrawing();
            ClearBackground(BLACK);
            DrawText("Dati di gioco mancanti.", 40, 200, 30, RED);
            DrawText("Esegui tools/prepare.sh e copia gamedata/ in", 40, 250, 20, WHITE);
            DrawText(vs_path(""), 40, 280, 20, YELLOW);
            EndDrawing();
        }
        CloseWindow();
        return 1;
    }
    SaveSystem_Load();  // PreloadScene: salvataggio (o i default) applicato alle tabelle, poi la lingua
    Lang_init();
    vs_load_font();
    trophies_init();
    Trophies_sync();

    // Prova automatica sul desktop: VS_SCENE=game (partita diretta), VS_MENU=main|chars|stages (salta l'intro), VS_SHOT=secondi -> shot.png ed esce.
    const char *env_scene = getenv("VS_SCENE"), *env_shot = getenv("VS_SHOT");
    // Sulla Vita niente ambiente: <dati>/scene.req con il nome della scena (consumato all'avvio).
    static char scene_req[16];
    char *req = env_scene ? NULL : LoadFileText(vs_path("scene.req"));
    if (req) {
        sscanf(req, "%15s", scene_req);
        UnloadFileText(req);
        remove(vs_path("scene.req"));
        env_scene = scene_req;
    }
    float shot_at = env_shot ? (float)atof(env_shot) : -1, run_t = 0;
    if (env_scene && !strcmp(env_scene, "game")) { game_start(getenv("VS_CHAR") ? getenv("VS_CHAR") : "ANTONIO"); scene_set(SCENE_GAME); }  // VS_CHAR: personaggio di prova
    else { menu_start(); scene_set(SCENE_MENU); }

    int prof = getenv("VS_PROF") != NULL;
    double p_up = 0, p_dr = 0, p_fr = 0, m_up = 0, m_dr = 0, m_fr = 0, t_prev = GetTime();
    int p_n = 0;
    while (!WindowShouldClose()) {
        double t0 = GetTime();
        float dt = det_on ? 1.0f / 60 : GetFrameTime();
        if (dt > 0.1f) dt = 0.1f;
        int quit = 0, net_stepped = 0;
        if (scene == SCENE_MENU) {
            int r = menu_update(dt);
            if (r > 0) scene_set(SCENE_GAME);  // LaunchMainScene
            quit = r < 0;                      // Integration.closeGame
        } else if (net_on) {  // lockstep: si simula solo quando ci sono gli input di entrambe le console
            while (net_wants_input(det_frame)) game_net_submit();
            net_stepped = net_tick(det_frame);
            net_input_frame = det_frame, net_in_sim = net_stepped;
            int ended = net_stepped && game_update(dt);
            net_in_sim = 0;
            if (ended || net_lost()) {
                game_net_end();
                menu_start();
                scene_set(SCENE_MENU);
            }
            if (det_frame % 600 == 0 && net_stepped) net_report(det_frame);
        } else if (game_update(dt)) {
            menu_start();  // StartFromPostGame -> IntroScene
            scene_set(SCENE_MENU);
        }
        if (quit) break;
        pad_log();
        if (det_on && scene == SCENE_GAME && (!net_on || net_stepped)) det_log();
        double t1 = GetTime();
        BeginDrawing();
        ClearBackground(BLACK);
        if (scene == SCENE_MENU) menu_draw();
        else {
            game_context_set(net_on ? net_role : 0);  // il proprio giocatore: HUD e schede
            game_draw();
            game_context_set(0);
        }
        double t2 = GetTime();
        if (vs_finish_mode() & 2) rlDrawRenderBatchActive(), glFinish();  // VS_FINISH=swap|both: tutto disegnato prima dello swap
        EndDrawing();
        probe_frame(scene == SCENE_GAME, dt);
        if (prof) {  // VS_PROF: medie e massimi ogni 60 frame (logica, disegno, frame intero = anche l'attesa della GPU)
            double t3 = GetTime(), up = t1 - t0, dr = t2 - t1, fr = t3 - t_prev;
            t_prev = t3;
#ifdef __vita__
            static unsigned g_sum, g_max, g_from;  // scene in coda alla GPU dopo lo scambio (0 = tiene il passo), scene per frame
            unsigned g = query_fence.value - *query_fence.address;
            if (!g_from) g_from = query_fence.value;
            g_sum += g;
            if (g > g_max) g_max = g;
#endif
            p_up += up, p_dr += dr, p_fr += fr, p_n++;
            if (up > m_up) m_up = up;
            if (dr > m_dr) m_dr = dr;
            if (fr > m_fr) m_fr = fr;
            if (p_n == 60) {
                char line[320], gpu[64] = "";
#ifdef __vita__
                snprintf(gpu, sizeof(gpu), " gpu coda %.1f/%u scene %.1f vsync %u", g_sum / 60.0, g_max, (query_fence.value - g_from) / 60.0, (unsigned)vsync_interval);
                g_sum = g_max = 0, g_from = query_fence.value;
#endif
                snprintf(line, sizeof(line), "t=%.0f fin=%d logica %.1f/%.1f disegno %.1f/%.1f frame %.1f/%.1f ms%s | %s\n", run_t, vs_finish_mode(),
                         1000 * p_up / 60, 1000 * m_up, 1000 * p_dr / 60, 1000 * m_dr, 1000 * p_fr / 60, 1000 * m_fr, gpu, game_prof());
                fputs(line, stderr);
                FILE *f = fopen(vs_path("prof.txt"), "a");
                if (f) fputs(line, f), fclose(f);
                p_up = p_dr = p_fr = m_up = m_dr = m_fr = 0, p_n = 0;
            }
        }
        run_t += dt;
        if (shot_at >= 0 && run_t > shot_at) {
            TakeScreenshot("shot.png");
            break;
        }
        // Screenshot su richiesta (sviluppo, anche sulla Vita): crea <dati>/shot.req -> <dati>/shot.png
        static int frame_no;
        if (++frame_no % 30 == 0 && FileExists(vs_path("shot.req"))) {
            remove(vs_path("shot.req"));
            Image im = LoadImageFromScreen();
            ExportImage(im, vs_path("shot.png"));
            UnloadImage(im);
        }
    }
    CloseWindow();
    return 0;
}
