/* rekordbox launcher for the SC LIVE 4 (EngineX mod).
 *
 *   rekordbox-launcher --serve    runs on port 8093 (RB_PORT) for the mod's whole life:
 *       GET  /          the setup and control page
 *       GET  /status    {"state":..., "message":..., "pack":...}
 *       POST /start     with a pack installed: stop Engine and run rekordbox
 *                       without one: look for rekordbox-pack.tgz on a USB drive and install it
 *       POST /stop      quit rekordbox (Engine comes back)
 *       POST /upload    the raw bytes of rekordbox-pack.tgz; it gets checked and installed
 *
 * The work itself is done by rekordbox.sh next to this binary (session, end, setup, install).
 * While rekordbox runs this watches the control surface: holding SHIFT and BACK together for
 * two seconds quits it. The state file /tmp/rekordbox/state is written by rekordbox.sh:
 * line 1 the state, line 2 a message for people.
 *
 * Built static for 32-bit ARM (hard float); see build.sh. No ALSA library: the sequencer is
 * read through its kernel interface, the same way the rblive4 shims do it.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <sound/asequencer.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef VERSION
#define VERSION "dev"
#endif

#define STATE_FILE "/tmp/rekordbox/state"
#define EMU_MIDI "/tmp/emu/midi-in"

static char script[600];
static char upload_path[600];
static char entry_path[600]; /* StartRekordbox.qml, served to the About tab */
static pid_t worker; /* rekordbox.sh session / setup / install, one at a time */
static volatile int watching;

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
    va_list ap;
    time_t t = time(NULL);
    char ts[32];
    strftime(ts, sizeof ts, "%F %T", localtime(&t));
    fprintf(stderr, "%s launcher: ", ts);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static int busy(void) {
    if (worker > 0 && waitpid(worker, NULL, WNOHANG) == 0) return 1;
    worker = 0;
    return 0;
}

static void run_script(const char *what, const char *arg) {
    if (busy()) return;
    worker = fork();
    if (worker == 0) {
        setsid();
        execl("/bin/sh", "sh", script, what, arg, (char *)NULL);
        _exit(127);
    }
    logf_("rekordbox.sh %s %s (pid %d)", what, arg ? arg : "", worker);
}

/* "sh rekordbox.sh end" quits rekordbox; the session then brings Engine back. */
static void end_session(void) {
    pid_t p = fork();
    if (p == 0) { execl("/bin/sh", "sh", script, "end", (char *)NULL); _exit(127); }
    if (p > 0) waitpid(p, NULL, 0);
}

static void read_state(char *st, size_t sn, char *msg, size_t mn) {
    snprintf(st, sn, "unknown");
    msg[0] = 0;
    FILE *f = fopen(STATE_FILE, "r");
    if (!f) return;
    if (fgets(st, (int)sn, f)) st[strcspn(st, "\r\n")] = 0;
    if (fgets(msg, (int)mn, f)) msg[strcspn(msg, "\r\n")] = 0;
    fclose(f);
}

static void json_str(char *out, size_t n, const char *s) {
    size_t k = 0;
    for (; *s && k + 7 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[k++] = '\\'; out[k++] = (char)c; }
        else if (c < 0x20) k += (size_t)snprintf(out + k, n - k, "\\u%04x", c);
        else out[k++] = (char)c;
    }
    out[k] = 0;
}

/* ------------------------------------------------------------------ SHIFT + BACK */

static int midi_note(const uint8_t st, uint8_t d1, uint8_t d2, int *shift, int *back) {
    int type = st & 0xF0, ch = st & 0x0F;
    if (type != 0x90 && type != 0x80) return 0;
    int on = type == 0x90 && d2 > 0;
    if ((ch == 4 || ch == 5) && d1 == 28) { shift[ch - 4] = on; return 1; }
    if (ch == 15 && d1 == 3) { *back = on; return 1; }
    return 0;
}

/* The emulator's panel writes the deck's MIDI as hex lines to /tmp/emu/midi-in. */
static int emu_open(void) {
    if (access(EMU_MIDI, F_OK)) return -1;
    int fd = open(EMU_MIDI, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    return fd;
}

static int seq_open(void) {
    int fd = open("/dev/snd/seq", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    int me = -1;
    struct snd_seq_client_info ci;
    struct snd_seq_port_info pi;
    if (ioctl(fd, SNDRV_SEQ_IOCTL_CLIENT_ID, &me) < 0) goto fail;
    memset(&ci, 0, sizeof ci);
    ci.client = me;
    ci.type = USER_CLIENT;
    snprintf(ci.name, sizeof ci.name, "rekordbox-launcher");
    if (ioctl(fd, SNDRV_SEQ_IOCTL_SET_CLIENT_INFO, &ci) < 0) goto fail;
    memset(&pi, 0, sizeof pi);
    pi.addr.client = (unsigned char)me;
    snprintf(pi.name, sizeof pi.name, "exit combo");
    pi.capability = SNDRV_SEQ_PORT_CAP_WRITE | SNDRV_SEQ_PORT_CAP_SUBS_WRITE;
    pi.type = SNDRV_SEQ_PORT_TYPE_MIDI_GENERIC | SNDRV_SEQ_PORT_TYPE_APPLICATION;
    if (ioctl(fd, SNDRV_SEQ_IOCTL_CREATE_PORT, &pi) < 0) goto fail;
    /* every hardware port that sends: on the deck that's the control surface (16:0) */
    int subs = 0;
    struct snd_seq_client_info c;
    memset(&c, 0, sizeof c);
    c.client = -1;
    while (ioctl(fd, SNDRV_SEQ_IOCTL_QUERY_NEXT_CLIENT, &c) == 0) {
        if (c.client == me || c.client == 0) continue;
        struct snd_seq_port_info p;
        memset(&p, 0, sizeof p);
        p.addr.client = (unsigned char)c.client;
        p.addr.port = (unsigned char)-1;
        while (ioctl(fd, SNDRV_SEQ_IOCTL_QUERY_NEXT_PORT, &p) == 0) {
            unsigned need = SNDRV_SEQ_PORT_CAP_READ | SNDRV_SEQ_PORT_CAP_SUBS_READ;
            if ((p.capability & need) == need && (p.type & SNDRV_SEQ_PORT_TYPE_HARDWARE)) {
                struct snd_seq_port_subscribe s;
                memset(&s, 0, sizeof s);
                s.sender = p.addr;
                s.dest = pi.addr;
                s.queue = SNDRV_SEQ_QUEUE_DIRECT;
                if (ioctl(fd, SNDRV_SEQ_IOCTL_SUBSCRIBE_PORT, &s) == 0) {
                    subs++;
                    logf_("watching %d:%d (%s) for SHIFT + BACK", p.addr.client, p.addr.port, c.name);
                }
            }
        }
    }
    if (subs) return fd;
fail:
    close(fd);
    return -1;
}

static void *watch(void *arg) {
    (void)arg;
    int shift[2] = {0, 0}, back = 0, emu = 0;
    int fd = emu_open();
    if (fd >= 0) emu = 1;
    else fd = seq_open();
    if (fd < 0) { logf_("can't read the control surface; SHIFT + BACK won't work this time"); return NULL; }
    char line[512];
    size_t ll = 0;
    uint8_t rs = 0, d[2];
    int have = 0;
    struct timespec held = {0, 0};
    while (watching) {
        struct timeval tv = {0, 100000};
        fd_set r;
        FD_ZERO(&r);
        FD_SET(fd, &r);
        if (select(fd + 1, &r, NULL, NULL, &tv) > 0) {
            if (emu) {
                ssize_t n = read(fd, line + ll, sizeof line - 1 - ll);
                if (n > 0) ll += (size_t)n;
                line[ll] = 0;
                char *nl;
                while ((nl = strchr(line, '\n'))) {
                    *nl = 0;
                    char *p = line, *e;
                    for (long v; (v = strtol(p, &e, 16)), e != p; p = e) {
                        uint8_t b = (uint8_t)v;
                        if (b & 0x80) { rs = b; have = 0; continue; }
                        if (!rs) continue;
                        d[have++] = b;
                        if (have == 2) { midi_note(rs, d[0], d[1], shift, &back); have = 0; }
                    }
                    size_t used = (size_t)(nl + 1 - line);
                    memmove(line, nl + 1, ll - used + 1);
                    ll -= used;
                }
                if (ll >= sizeof line - 2) ll = 0;
            } else {
                unsigned char buf[1024];
                ssize_t n = read(fd, buf, sizeof buf);
                for (ssize_t o = 0; n > 0 && o + (ssize_t)sizeof(struct snd_seq_event) <= n;) {
                    struct snd_seq_event *ev = (struct snd_seq_event *)(buf + o);
                    if (ev->type == SNDRV_SEQ_EVENT_NOTEON || ev->type == SNDRV_SEQ_EVENT_NOTEOFF) {
                        uint8_t st = (uint8_t)((ev->type == SNDRV_SEQ_EVENT_NOTEON ? 0x90 : 0x80) | (ev->data.note.channel & 0x0F));
                        midi_note(st, ev->data.note.note, ev->data.note.velocity, shift, &back);
                    }
                    o += (ssize_t)sizeof(struct snd_seq_event);
                    if ((ev->flags & SNDRV_SEQ_EVENT_LENGTH_MASK) == SNDRV_SEQ_EVENT_LENGTH_VARIABLE)
                        o += ev->data.ext.len;
                }
            }
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((shift[0] || shift[1]) && back) {
            if (!held.tv_sec) held = now;
            else if (now.tv_sec - held.tv_sec >= 2) {
                logf_("SHIFT + BACK held: quitting rekordbox");
                end_session();
                held.tv_sec = 0;
                shift[0] = shift[1] = back = 0;
            }
        } else held.tv_sec = 0;
    }
    close(fd);
    return NULL;
}

/* Watch the controls only while a session runs. */
static void update_watch(void) {
    static pthread_t th;
    static int on;
    char st[64], msg[512];
    read_state(st, sizeof st, msg, sizeof msg);
    int want = busy() && (!strcmp(st, "starting") || !strcmp(st, "running"));
    if (want && !on) {
        watching = 1;
        on = !pthread_create(&th, NULL, watch, NULL);
    } else if (!want && on) {
        watching = 0;
        pthread_join(th, NULL);
        on = 0;
    }
}

/* ------------------------------------------------------------------ HTTP */

static const char PAGE[] =
#include "page.inc"
    ;

static void reply(int fd, const char *status, const char *type, const char *body) {
    char h[256];
    int n = snprintf(h, sizeof h,
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                     "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
                     status, type, strlen(body));
    if (write(fd, h, (size_t)n) < 0 || write(fd, body, strlen(body)) < 0) {}
}

static int installed(void) {
    const char *r = getenv("RB_ROOT");
    char p[600];
    snprintf(p, sizeof p, "%s/.rekordbox-pack", r && *r ? r : "/data/rbx3-run");
    return access(p, F_OK) == 0;
}

static void status_json(char *out, size_t n) {
    char st[64], msg[512], m[1100];
    read_state(st, sizeof st, msg, sizeof msg);
    if (busy() && (!strcmp(st, "missing") || !strcmp(st, "ready") || !strcmp(st, "error"))) snprintf(st, sizeof st, "working");
    json_str(m, sizeof m, msg);
    snprintf(out, n, "{\"state\":\"%s\",\"message\":\"%s\",\"installed\":%s,\"launcher\":\"%s\"}", st, m,
             installed() ? "true" : "false", VERSION);
}

/* POST /upload: the body is the pack, Content-Length bytes, some of them already read. */
static void take_upload(int c, char *req, size_t got, size_t head_len) {
    const char *cl = strcasestr(req, "\r\ncontent-length:");
    long long want = cl ? atoll(cl + 17) : -1;
    if (want <= 0) { reply(c, "411 Length Required", "application/json", "{\"error\":\"no Content-Length\"}"); return; }
    if (busy()) { reply(c, "409 Conflict", "application/json", "{\"error\":\"busy, try again when it's done\"}"); return; }
    char part[640];
    snprintf(part, sizeof part, "%s.part", upload_path);
    int out = open(part, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0) { reply(c, "500 Internal Server Error", "application/json", "{\"error\":\"can't write to /data\"}"); return; }
    struct timeval tv = {30, 0};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    long long have = (long long)(got - head_len);
    int ok = 1;
    if (have > 0 && write(out, req + head_len, (size_t)have) != have) ok = 0;
    static char buf[65536];
    while (ok && have < want) {
        ssize_t n = read(c, buf, sizeof buf);
        if (n <= 0) { ok = 0; break; }
        if (write(out, buf, (size_t)n) != n) ok = 0;
        have += n;
    }
    if (close(out) || !ok || have != want) {
        unlink(part);
        logf_("upload failed after %lld of %lld bytes", have, want);
        reply(c, "400 Bad Request", "application/json", "{\"error\":\"the upload didn't finish (out of space, or the connection dropped)\"}");
        return;
    }
    rename(part, upload_path);
    logf_("received %lld bytes", have);
    run_script("install", upload_path);
    reply(c, "200 OK", "application/json", "{\"ok\":true}");
}

static int serve(void) {
    signal(SIGPIPE, SIG_IGN);
    const char *ps = getenv("RB_PORT");
    int port = ps && *ps ? atoi(ps) : 8093;
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 8)) { perror("launcher: listen"); return 1; }
    logf_("version %s on port %d", VERSION, port);
    for (;;) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(s, &r);
        struct timeval tick = {0, 500000};
        update_watch();
        if (select(s + 1, &r, NULL, NULL, &tick) <= 0) continue;
        int c = accept4(s, NULL, NULL, SOCK_CLOEXEC);
        if (c < 0) continue;
        struct timeval tv = {5, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        static char req[16384];
        size_t got = 0;
        char *end = NULL;
        while (got < sizeof req - 1) {
            ssize_t n = read(c, req + got, sizeof req - 1 - got);
            if (n <= 0) break;
            got += (size_t)n;
            req[got] = 0;
            if ((end = strstr(req, "\r\n\r\n"))) break;
        }
        if (end) {
            char m[8] = "", path[128] = "", body[1400];
            sscanf(req, "%7s %127s", m, path);
            char *q = strchr(path, '?');
            if (q) *q = 0;
            if (!strcmp(m, "POST") && !strcmp(path, "/start")) {
                char st[64], msg[512];
                read_state(st, sizeof st, msg, sizeof msg);
                if (!busy() && (!strcmp(st, "ready") || (!strcmp(st, "error") && installed()))) run_script("session", NULL);
                else if (!strcmp(st, "missing") || !strcmp(st, "error")) run_script("setup", NULL);
                status_json(body, sizeof body);
                reply(c, "200 OK", "application/json", body);
            } else if (!strcmp(m, "POST") && !strcmp(path, "/stop")) {
                end_session();
                reply(c, "200 OK", "application/json", "{\"ok\":true}");
            } else if (!strcmp(m, "POST") && !strcmp(path, "/upload")) {
                take_upload(c, req, got, (size_t)(end + 4 - req));
            } else if (!strcmp(path, "/status")) {
                status_json(body, sizeof body);
                reply(c, "200 OK", "application/json", body);
            } else if (!strcmp(m, "GET") && (!strcmp(path, "/") || !strcmp(path, "/index.html"))) {
                reply(c, "200 OK", "text/html; charset=utf-8", PAGE);
            } else if (!strcmp(m, "GET") && !strcmp(path, "/entry.qml")) {
                static char qml[16384];
                FILE *f = fopen(entry_path, "r");
                size_t k = f ? fread(qml, 1, sizeof qml - 1, f) : 0;
                if (f) fclose(f);
                qml[k] = 0;
                if (k) reply(c, "200 OK", "text/plain; charset=utf-8", qml);
                else reply(c, "404 Not Found", "text/plain", "no entry\n");
            } else reply(c, "404 Not Found", "text/plain", "not found\n");
        }
        close(c);
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--version")) { puts(VERSION); return 0; }
    char self[512];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) snprintf(self, sizeof self, "%s", argv[0]); else self[n] = 0;
    const char *dir = getenv("RB_MOD_DIR");
    if (dir && *dir) snprintf(script, sizeof script, "%s/rekordbox.sh", dir);
    else {
        char *sl = strrchr(self, '/');
        if (sl) *sl = 0;
        snprintf(script, sizeof script, "%s/rekordbox.sh", sl ? self : ".");
    }
    snprintf(entry_path, sizeof entry_path, "%.*s/StartRekordbox.qml", (int)(strrchr(script, '/') - script), script);
    const char *up = getenv("RB_UPLOAD");
    snprintf(upload_path, sizeof upload_path, "%s", up && *up ? up : "/data/rekordbox-pack.tgz");
    mkdir("/tmp/rekordbox", 0755);
    if (argc > 1 && !strcmp(argv[1], "--serve")) {
        pid_t p = fork(); /* fill in the state before the first request */
        if (p == 0) { execl("/bin/sh", "sh", script, "check", (char *)NULL); _exit(127); }
        if (p > 0) waitpid(p, NULL, 0);
        return serve();
    }
    fprintf(stderr, "usage: %s --serve | --version\n", argv[0]);
    return 2;
}
