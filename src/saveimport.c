// Import of a PC save: while the screen is open the game serves one small page on the local network,
// where the browser on the computer uploads SaveData.sav. Nothing is written here: vs_game.c validates
// the bytes and asks for confirmation. One client at a time, no file paths involved.
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "saveimport.h"

#ifdef __vita__
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#endif

#define SI_PORT 8080
#define SI_MAX (4 * 1024 * 1024)
#define SI_HEAD 8192

static const char PAGE[] =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Vampire Survivors - PS Vita</title>"
    "<style>body{font:16px sans-serif;background:#1b0a0e;color:#eee;max-width:620px;margin:40px auto;padding:0 16px}"
    "h1{color:#ffcd50;font-size:22px}code{background:#000;padding:2px 5px;word-break:break-all}"
    "button{font-size:16px;padding:10px 18px;background:#9c0f1c;color:#fff;border:0;margin-top:12px}</style>"
    "<h1>Send your save to the Vita</h1>"
    "<p>Choose your save from the computer. Both the current version and the <b>old-engine</b> build work; content this port does not have yet is skipped.</p>"
    "<p>Current version, Steam Cloud: <code>Steam/userdata/&lt;id&gt;/1794680/remote/SaveData</code></p>"
    "<p>Old-engine, <code>SaveData.sav</code>:<br>Windows: <code>%APPDATA%\\Vampire_Survivors_&lt;id&gt;</code><br>"
    "macOS: <code>~/Library/Application Support/Vampire_Survivors_&lt;id&gt;</code><br>"
    "Linux: <code>~/.config/Vampire_Survivors_&lt;id&gt;</code></p>"
    "<input id=f type=file><br><button onclick=send()>Send</button><p id=r></p>"
    "<script>async function send(){const r=document.getElementById('r'),x=document.getElementById('f').files[0];"
    "if(!x){r.textContent='Choose a file first.';return}r.textContent='Sending...';"
    "try{const a=await fetch('/upload',{method:'POST',body:x});r.textContent=await a.text()}"
    "catch(e){r.textContent='The Vita did not answer.'}}</script>";

static int lfd = -1, cfd = -1;
static char head[SI_HEAD];
static size_t head_n, body_need, body_n;
static int in_body;
static char *body;
static char *received;  // complete upload, waiting for saveimport_take
static char ip[32] = "?";

static void set_nonblock(int fd) {
#ifdef __vita__
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NONBLOCK, &on, sizeof(on));
#else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
}

static void close_client(void) {
    if (cfd >= 0) close(cfd);
    cfd = -1, head_n = 0, in_body = 0, body_need = body_n = 0;
    free(body);
    body = NULL;
}

static void reply(const char *status, const char *type, const char *text) {
    char h[256];
    size_t len = strlen(text);
    int n = snprintf(h, sizeof(h), "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", status, type, (int)len);
    // A single send: the page fits the socket buffer.
    send(cfd, h, n, 0);
    send(cfd, text, len, 0);
    close_client();
}

int saveimport_start(void) {
    saveimport_stop();
#ifdef __vita__
    static char mem[256 * 1024];
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    SceNetInitParam p = { mem, sizeof(mem), 0 };
    sceNetInit(&p);  // already initialised is fine
    sceNetCtlInit();
    SceNetCtlInfo info;
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) < 0) return 0;
    snprintf(ip, sizeof(ip), "%s", info.ip_address);
#else
    snprintf(ip, sizeof(ip), "localhost");
#endif
    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) return 0;
    int on = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET, a.sin_port = htons(SI_PORT), a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(lfd, 2) < 0) {
        saveimport_stop();
        return 0;
    }
    set_nonblock(lfd);
    return 1;
}

void saveimport_stop(void) {
    close_client();
    if (lfd >= 0) close(lfd);
    lfd = -1;
    free(received);
    received = NULL;
}

const char *saveimport_address(void) {
    static char s[64];
    snprintf(s, sizeof(s), "http://%s:%d", ip, SI_PORT);
    return s;
}

static const char *find_header(const char *h, const char *name) {  // name in lower case
    size_t n = strlen(name);
    for (const char *p = h; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && (p[i] | 0x20) == (name[i] | 0x20)) i++;
        if (i == n) return p;
    }
    return NULL;
}

static void on_headers(void) {
    if (strncmp(head, "POST /upload", 12)) { reply("200 OK", "text/html", PAGE); return; }
    const char *c = find_header(head, "content-length:");
    long n = c ? strtol(c + 15, NULL, 10) : 0;
    if (n <= 0 || n > SI_MAX) { reply("413 Too Large", "text/plain", "That file is not a save."); return; }
    body = malloc(n + 1);
    if (!body) { reply("500 Error", "text/plain", "The Vita is out of memory."); return; }
    char *e = strstr(head, "\r\n\r\n") + 4;
    body_need = n;
    body_n = head_n - (e - head);
    if (body_n > body_need) body_n = body_need;
    memcpy(body, e, body_n);
    in_body = 1;
}

void saveimport_poll(void) {
    if (lfd < 0) return;
    if (cfd < 0) {
        cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) return;
        set_nonblock(cfd);
    }
    for (int spins = 0; spins < 64 && cfd >= 0; spins++) {
        if (!in_body) {
            ssize_t r = recv(cfd, head + head_n, SI_HEAD - 1 - head_n, 0);
            if (r == 0) { close_client(); return; }
            if (r < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK) close_client(); return; }
            head_n += r;
            head[head_n] = 0;
            if (strstr(head, "\r\n\r\n")) on_headers();
            else if (head_n >= SI_HEAD - 1) reply("400 Bad Request", "text/plain", "Bad request.");
            if (cfd < 0) return;
        }
        if (in_body) {
            if (body_n < body_need) {
                ssize_t r = recv(cfd, body + body_n, body_need - body_n, 0);
                if (r == 0) { close_client(); return; }
                if (r < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK) close_client(); return; }
                body_n += r;
            }
            if (body_n == body_need) {
                body[body_n] = 0;
                free(received);
                received = body;
                body = NULL;
                reply("200 OK", "text/plain", "Received. Now look at your Vita.");
                return;
            }
        }
    }
}

char *saveimport_take(void) {
    char *r = received;
    received = NULL;
    return r;
}

void saveimport_free(char *s) { free(s); }
