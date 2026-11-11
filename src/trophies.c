// PlayStation trophies. They only work with the NoTrpDrm plugin installed: without it the context
// cannot be created and the game simply carries on with its own achievements.
// A set holds at most 128 trophies, so the achievements are split over two sets.
#include <string.h>
#include "trophies.h"
#include "trophy_ids.h"

// Achievement index -> set and id. Set 0 has the platinum at id 0.
static int trophy_find(const char *key, int *set) {
    for (int i = 0; i < TROPHY_COUNT; i++)
        if (!strcmp(TROPHY_KEYS[i], key)) {
            *set = i >= TROPHY_SET0;
            return *set ? i - TROPHY_SET0 : i + 1;
        }
    return -1;
}

#ifdef __vita__
#include <psp2/common_dialog.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <vitaGL.h>

typedef struct {
    int sdkVersion;
    SceCommonDialogParam commonParam;
    int context;
    int options;
    unsigned char reserved[128];
} TrophySetupParam;

int sceNpTrophyInit(void *unk);
int sceNpTrophyCreateContext(int *context, char *commId, char *commSign, unsigned long long options);
int sceNpTrophySetupDialogInit(TrophySetupParam *param);
SceCommonDialogStatus sceNpTrophySetupDialogGetStatus(void);
int sceNpTrophySetupDialogTerm(void);
int sceNpTrophyCreateHandle(int *handle);
int sceNpTrophyDestroyHandle(int handle);
int sceNpTrophyUnlockTrophy(int ctx, int handle, int id, int *platinum);
int sceNpTrophyGetTrophyUnlockState(int ctx, int handle, void *state, unsigned int *count);

#define SETS 2
static struct { int ctx, ready; unsigned int state[4]; } sets[SETS];  // 128 bits, the system's limit
static int queue[256];
static volatile int q_head, q_tail;
static SceUID sema;

static int unlocker(SceSize args, void *argp) {
    (void)args, (void)argp;
    for (;;) {
        sceKernelWaitSema(sema, 1, NULL);
        int item = queue[q_tail % 256], h, plat = -1;
        q_tail++;
        if (sceNpTrophyCreateHandle(&h) < 0) continue;
        sceNpTrophyUnlockTrophy(sets[item >> 8].ctx, h, item & 0xff, &plat);
        sceNpTrophyDestroyHandle(h);
    }
    return 0;
}

static int set_open(int s) {
    // SceNpCommunicationId: the 9-character id, a NUL, the set number, a reserved byte
    char comm_id[12] = "VSPH00001";
    static char signature[160] = { 0xb9, 0xdd, 0xe1, 0x3b, 0x01, 0x00 };
    comm_id[10] = (char)s;
    if (sceNpTrophyCreateContext(&sets[s].ctx, comm_id, signature, 0) < 0) return 0;  // no NoTrpDrm
    TrophySetupParam p;
    memset(&p, 0, sizeof(p));
    _sceCommonDialogSetMagicNumber(&p.commonParam);
    p.sdkVersion = PSP2_SDK_VERSION;
    p.context = sets[s].ctx;
    if (sceNpTrophySetupDialogInit(&p) < 0) return 0;
    while (sceNpTrophySetupDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING) {
        glClear(GL_COLOR_BUFFER_BIT);
        vglSwapBuffers(GL_TRUE);  // GL_TRUE: this is what makes the system dialog advance
    }
    sceNpTrophySetupDialogTerm();
    int h;
    unsigned int n;
    if (sceNpTrophyCreateHandle(&h) >= 0) {
        sceNpTrophyGetTrophyUnlockState(sets[s].ctx, h, sets[s].state, &n);
        sceNpTrophyDestroyHandle(h);
    }
    sets[s].ready = 1;
    return 1;
}

void trophies_init(void) {
    sceSysmoduleLoadModule(SCE_SYSMODULE_NP_TROPHY);
    sceNpTrophyInit(NULL);
    for (int s = 0; s < SETS; s++)
        if (!set_open(s)) break;
    if (!sets[0].ready) return;
    sema = sceKernelCreateSema("trophies", 0, 0, 256, NULL);
    SceUID t = sceKernelCreateThread("trophies", unlocker, 0x10000100, 0x10000, 0, 0, NULL);
    sceKernelStartThread(t, 0, NULL);
}

void trophies_unlock(const char *key) {
    int s, id = trophy_find(key, &s);
    if (id < 0 || id >= 128 || !sets[s].ready) return;
    if (sets[s].state[id >> 5] & (1u << (id & 31))) return;
    sets[s].state[id >> 5] |= 1u << (id & 31);
    if (q_head - q_tail >= 256) return;
    queue[q_head % 256] = s << 8 | id;
    q_head++;
    sceKernelSignalSema(sema, 1);
}

int trophies_active(void) { return sets[0].ready; }
#else
int trophies_active(void) { return 0; }
void trophies_init(void) {}
void trophies_unlock(const char *key) { int s; (void)trophy_find(key, &s); }
#endif
