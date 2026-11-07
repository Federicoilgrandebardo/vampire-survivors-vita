// Lockstep: le due console simulano la stessa partita e si scambiano solo gli input, con qualche frame di anticipo.
// Un frame parte quando ci sono gli input di entrambe. Ogni pacchetto ripete gli ultimi input, cosi' UDP puo' perderne senza danni,
// e porta l'hash dello stato: se le due simulazioni divergono si vede subito.
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "net.h"
#include "vs.h"
#ifdef __vita__
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <psp2/kernel/threadmgr.h>
#endif

#define NET_PORT 7711
#define NET_DELAY 4    // frame di anticipo degli input
#define NET_RING 1024
#define NET_RESEND 24  // input ripetuti in ogni pacchetto
#define NET_HASHES 8
#define NET_MAGIC 0x5653u

int net_on, net_role;
static double last_rx;
static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }
static int fd = -1;
static struct sockaddr_in peer_addr;
static int peer_known;
static NetMove moves[2][NET_RING];
static unsigned have[2];        // frame fino a cui (esclusi) gli input sono noti
static unsigned hashes[NET_RING], hash_done;
static unsigned hash_checked, desync_at, desyncs, stalls, lost_percent;
static int desynced;

typedef struct {
    unsigned short magic;
    unsigned char kind, count;  // kind: 0 saluto, 1 risposta col seme, 2 input
    unsigned first, seed, hash_first, hash_done, hash[NET_HASHES];
    NetMove m[NET_RESEND];
} Packet;

static void sleep_ms(int ms) {
#ifdef __vita__
    sceKernelDelayThread(ms * 1000);
#else
    usleep(ms * 1000);
#endif
}
static void net_system_init(void) {
#ifdef __vita__
    static char mem[256 * 1024];
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    SceNetInitParam p = { mem, sizeof(mem), 0 };
    sceNetInit(&p);  // gia' inizializzata va bene
    sceNetCtlInit();
#endif
}
static void net_send(const Packet *p) {
    if (peer_known && (!lost_percent || (unsigned)(rand() % 100) >= lost_percent)) sendto(fd, p, sizeof(*p), 0, (struct sockaddr *)&peer_addr, sizeof(peer_addr));
}
int net_open(const char *peer, unsigned *seed) {
    net_system_init();
    if (getenv("VS_NETLOSS")) lost_percent = (unsigned)atoi(getenv("VS_NETLOSS"));
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    struct sockaddr_in me;
    memset(&me, 0, sizeof(me));
    me.sin_family = AF_INET, me.sin_addr.s_addr = htonl(INADDR_ANY), me.sin_port = htons(peer ? 0 : NET_PORT);
    if (bind(fd, (struct sockaddr *)&me, sizeof(me)) < 0) return 0;
    net_role = peer ? 1 : 0;
    if (peer) {
        memset(&peer_addr, 0, sizeof(peer_addr));
        peer_addr.sin_family = AF_INET, peer_addr.sin_port = htons(NET_PORT), peer_addr.sin_addr.s_addr = inet_addr(peer);
        peer_known = 1;
    }
    for (int tries = 0; tries < 3000; tries++) {  // 60 s
        Packet p = { NET_MAGIC }, r;
        if (peer) { p.kind = 0; sendto(fd, &p, sizeof(p), 0, (struct sockaddr *)&peer_addr, sizeof(peer_addr)); }
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        while (recvfrom(fd, &r, sizeof(r), 0, (struct sockaddr *)&from, &fl) == (int)sizeof(r)) {
            if (r.magic != NET_MAGIC) continue;
            if (peer && r.kind == 1) { *seed = r.seed; net_begin(); return 1; }
            if (!peer && r.kind == 0) {
                peer_addr = from, peer_known = 1;
                p.kind = 1, p.seed = *seed;
                for (int i = 0; i < 20; i++) sendto(fd, &p, sizeof(p), 0, (struct sockaddr *)&peer_addr, sizeof(peer_addr));
                net_begin();
                return 1;
            }
        }
        sleep_ms(20);
    }
    return 0;
}
int net_in_sim, net_input_player;
unsigned net_input_frame;
static unsigned net_bits(int player, unsigned frame) { return moves[player][frame % NET_RING].buttons; }
int net_pad_down(int b) { return (net_bits(net_input_player, net_input_frame) >> b) & 1; }
static int net_was_down(int player, int b) { return net_input_frame ? (net_bits(player, net_input_frame - 1) >> b) & 1 : 0; }
int net_pad_pressed(int b) { return net_pad_down(b) && !net_was_down(net_input_player, b); }
int net_pad_released(int b) { return !net_pad_down(b) && net_was_down(net_input_player, b); }
float net_pad_axis(int a) { NetMove m = moves[net_input_player][net_input_frame % NET_RING]; return a == 0 ? m.x / 127.0f : a == 1 ? m.y / 127.0f : 0; }
int net_player_pressed(int player, int b) { return ((net_bits(player, net_input_frame) >> b) & 1) && !net_was_down(player, b); }
// --- Lobby ------------------------------------------------------------------------------------------------------------
#define NET_BEACON_PORT 7712
enum { LK_BEACON = 10, LK_HELLO, LK_WELCOME, LK_CHUNK, LK_ACK };
typedef struct {
    unsigned short magic;
    unsigned char kind, pad;
    unsigned seed, index, total, length;
    char data[1200];
} LobbyPacket;
static int lobby_host, lobby_polls, lobby_connected, go_received, go_total;
static char *go_text;
static void lobby_send(const LobbyPacket *p, const struct sockaddr_in *to) { sendto(fd, p, sizeof(*p), 0, (const struct sockaddr *)to, sizeof(*to)); }
static int lobby_bind(int port) {
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in me;
    memset(&me, 0, sizeof(me));
    me.sin_family = AF_INET, me.sin_addr.s_addr = htonl(INADDR_ANY), me.sin_port = htons(port);
    return bind(fd, (struct sockaddr *)&me, sizeof(me)) == 0;
}
int net_lobby_start(int host) {
    net_system_init();
    net_close();
    lobby_host = host, lobby_polls = 0, lobby_connected = 0, peer_known = 0, go_received = 0, go_total = 0;
    free(go_text), go_text = NULL;
    net_role = host ? 0 : 1;
    if (!lobby_bind(host ? NET_PORT : NET_BEACON_PORT)) return 0;
    const char *direct = getenv("VS_NETPEER");  // sviluppo: senza annunci
    if (direct && !host) {
        memset(&peer_addr, 0, sizeof(peer_addr));
        peer_addr.sin_family = AF_INET, peer_addr.sin_port = htons(NET_PORT), peer_addr.sin_addr.s_addr = inet_addr(direct);
        peer_known = 1;
    }
    return 1;
}
int net_lobby_poll(const char *hello, char *out, int out_size) {
    LobbyPacket r, p;
    memset(&p, 0, sizeof(p));
    p.magic = NET_MAGIC;
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    while (recvfrom(fd, &r, sizeof(r), 0, (struct sockaddr *)&from, &fl) == (int)sizeof(r)) {
        if (r.magic != NET_MAGIC) continue;
        if (!lobby_host && r.kind == LK_BEACON && !peer_known) {
            peer_addr = from, peer_known = 1;
            peer_addr.sin_port = htons(NET_PORT);
        }
        if (!lobby_host && r.kind == LK_WELCOME) lobby_connected = 1;
        if (lobby_host && r.kind == LK_HELLO) {
            peer_addr = from, peer_known = 1, lobby_connected = 1;
            snprintf(out, out_size, "%s", r.data);
            p.kind = LK_WELCOME;
            lobby_send(&p, &peer_addr);
        }
    }
    if (lobby_polls++ % 15 == 0) {
        if (lobby_host && !lobby_connected) {
            struct sockaddr_in all;
            memset(&all, 0, sizeof(all));
            all.sin_family = AF_INET, all.sin_port = htons(NET_BEACON_PORT), all.sin_addr.s_addr = htonl(INADDR_BROADCAST);
            p.kind = LK_BEACON;
            lobby_send(&p, &all);
        } else if (!lobby_host && peer_known && !lobby_connected) {
            p.kind = LK_HELLO;
            snprintf(p.data, sizeof(p.data), "%s", hello);
            lobby_send(&p, &peer_addr);
        }
    }
    return lobby_connected;
}
void net_lobby_resend(const char *hello) {  // chi si unisce, gia' collegato, ha cambiato personaggio: solo invio (i pezzi del via restano a wait_go)
    if (lobby_host || !peer_known) return;
    LobbyPacket p;
    memset(&p, 0, sizeof(p));
    p.magic = NET_MAGIC, p.kind = LK_HELLO;
    snprintf(p.data, sizeof(p.data), "%s", hello);
    lobby_send(&p, &peer_addr);
}
int net_lobby_send_go(unsigned seed, const char *text) {
    unsigned length = (unsigned)strlen(text) + 1, chunk = sizeof(((LobbyPacket *)0)->data), total = (length + chunk - 1) / chunk;
    for (unsigned i = 0; i < total; i++) {
        LobbyPacket p, r;
        memset(&p, 0, sizeof(p));
        p.magic = NET_MAGIC, p.kind = LK_CHUNK, p.seed = seed, p.index = i, p.total = total, p.length = length;
        memcpy(p.data, text + i * chunk, i == total - 1 ? length - i * chunk : chunk);
        int acked = 0;
        for (int tries = 0; tries < 500 && !acked; tries++) {  // 10 s
            lobby_send(&p, &peer_addr);
            sleep_ms(20);
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            while (recvfrom(fd, &r, sizeof(r), 0, (struct sockaddr *)&from, &fl) == (int)sizeof(r))
                if (r.magic == NET_MAGIC && r.kind == LK_ACK && r.index == i) acked = 1;
        }
        if (!acked) return 0;
    }
    return 1;
}
int net_lobby_wait_go(unsigned *seed, char **text) {
    LobbyPacket r;
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    unsigned chunk = sizeof(r.data);
    while (recvfrom(fd, &r, sizeof(r), 0, (struct sockaddr *)&from, &fl) == (int)sizeof(r)) {
        if (r.magic != NET_MAGIC || r.kind != LK_CHUNK) continue;
        if (!go_text) go_text = calloc(r.length + 1, 1);
        if ((int)r.index == go_received) {
            unsigned n = r.index == r.total - 1 ? r.length - r.index * chunk : chunk;
            memcpy(go_text + r.index * chunk, r.data, n);
            go_received++, go_total = (int)r.total, *seed = r.seed;
        }
        LobbyPacket a;
        memset(&a, 0, sizeof(a));
        a.magic = NET_MAGIC, a.kind = LK_ACK, a.index = r.index;
        for (int k = 0; k < 2; k++) lobby_send(&a, &peer_addr);
    }
    if (go_total && go_received == go_total) { *text = go_text; go_text = NULL; return 1; }
    return 0;
}
void net_begin(void) {
    memset(moves, 0, sizeof(moves)), memset(hashes, 0, sizeof(hashes));
    have[0] = have[1] = NET_DELAY;
    hash_done = hash_checked = desync_at = desyncs = stalls = 0, desynced = 0;
    net_input_player = 0, net_input_frame = 0, net_on = 1;
    last_rx = now_s();
}
void net_close(void) {
    net_on = 0;
    if (fd >= 0) close(fd);
    fd = -1, peer_known = 0;
}
int net_lost(void) { return now_s() - last_rx > 10; }
int net_wants_input(unsigned frame) { return have[net_role] < frame + NET_DELAY + 1; }
void net_submit(NetMove local) { moves[net_role][have[net_role] % NET_RING] = local, have[net_role]++; }
NetMove net_move(int player, unsigned frame) { return moves[player][frame % NET_RING]; }
void net_frame_hash(unsigned frame, unsigned hash) { hashes[frame % NET_RING] = hash, hash_done = frame + 1; }
int net_tick(unsigned frame) {
    Packet r;
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    int other = !net_role;
    while (recvfrom(fd, &r, sizeof(r), 0, (struct sockaddr *)&from, &fl) == (int)sizeof(r)) {
        if (r.magic != NET_MAGIC || r.kind != 2) continue;
        for (unsigned i = 0; i < r.count; i++) {
            unsigned f = r.first + i;
            if (f == have[other]) moves[other][f % NET_RING] = r.m[i], have[other]++;
            last_rx = now_s();
        }
        for (unsigned i = 0; i < NET_HASHES; i++) {
            unsigned f = r.hash_first + i;
            if (f == hash_checked && f < hash_done && f < r.hash_done) {
                if (r.hash[i] != hashes[f % NET_RING]) { if (!desynced) desynced = 1, desync_at = f; desyncs++; }
                hash_checked++;
            }
        }
    }
    Packet p = { NET_MAGIC };
    p.kind = 2;
    unsigned mine = have[net_role];
    p.first = mine > NET_RESEND ? mine - NET_RESEND : 0, p.count = (unsigned char)(mine - p.first);
    for (unsigned i = 0; i < p.count; i++) p.m[i] = moves[net_role][(p.first + i) % NET_RING];
    p.hash_first = hash_done > NET_HASHES ? hash_done - NET_HASHES : 0;
    p.hash_done = hash_done;
    for (unsigned i = 0; i < NET_HASHES; i++) p.hash[i] = hashes[(p.hash_first + i) % NET_RING];
    net_send(&p);
    int ready = have[0] > frame && have[1] > frame;
    if (!ready) stalls++;
    return ready;
}
void net_report(unsigned frame) {
    FILE *f = fopen(vs_path("net.log"), "a");
    if (!f) return;
    fprintf(f, "frame %u controllati %u fermi %u divergenze %u prima %d\n", frame, hash_checked, stalls, desyncs, desynced ? (int)desync_at : -1);
    fclose(f);
}
