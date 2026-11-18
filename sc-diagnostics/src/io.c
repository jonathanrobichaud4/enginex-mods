/* sc-diagnostics: touch, control surface MIDI and audio (see io.h).
 *
 * On the deck:
 *   touch   the evdev device with multitouch axes (Engine's eglfs reads the same one), mapped
 *           through the panel's native size and rotation onto the canvas
 *   MIDI    the control surface's raw MIDI device, the serial port Engine's scripts reach as
 *           hw:/dev/snd/by-path/platform-ff190000.serial (amidi), opened as /dev/snd/midiCxD0
 *   audio   the JP21 codec through ALSA: S32_LE, 44.1 kHz, its 8 outputs and its inputs
 * In the SC LIVE 4 emulator (EMU_FB set) touch and MIDI use its pipes instead, and audio goes
 * through its ALSA stand-in, preloaded as on Engine.
 * Overrides: DIAG_TOUCH (event device), DIAG_TOUCH_ROTATE, DIAG_MIDI (device), DIAG_PCM. */
#define _GNU_SOURCE
#include "io.h"
#include "gfx.h"
#include <alsa/asoundlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int emu(void) { const char *e = getenv("EMU_FB"); return e && *e; }

/* ================================================================ touch */
touch_t touches[MAXT];
int touch_fd = -1;
static char tname[96] = "none";
static int emu_touch_keep = -1;
static int ax_min, ax_max, ay_min, ay_max, mt, slot, trot;
static touch_t pend[MAXT];
static char ebuf[512];
static int elen;

const char *touch_name(void) { return tname; }

static int has_bit(const unsigned long *b, int n) { return (b[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long)))) & 1; }

int touch_open(void) {
    if (emu()) {
        const char *p = getenv("EMU_INPUT");
        if (!p) p = "/tmp/emu/input";
        struct stat st;
        /* reuse the emulator's pipe (replacing it would leave Engine reading a deleted one) */
        if (stat(p, &st) || !S_ISFIFO(st.st_mode)) { unlink(p); if (mkfifo(p, 0666) && errno != EEXIST) return -1; }
        touch_fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        emu_touch_keep = open(p, O_WRONLY | O_NONBLOCK | O_CLOEXEC); /* never see EOF */
        snprintf(tname, sizeof tname, "emulator input pipe");
        return touch_fd < 0 ? -1 : 0;
    }
    const char *want = getenv("DIAG_TOUCH");
    const char *tr = getenv("DIAG_TOUCH_ROTATE");
    for (int i = 0; i < 32 && touch_fd < 0; i++) {
        char path[64];
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        if (want && *want && strcmp(want, path)) continue;
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned long abs_bits[(ABS_MAX + 1) / (8 * sizeof(long)) + 1] = {0};
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs_bits), abs_bits);
        int ismt = has_bit(abs_bits, ABS_MT_POSITION_X), isst = has_bit(abs_bits, ABS_X);
        if (!ismt && !isst) { close(fd); continue; }
        struct input_absinfo ix, iy;
        ioctl(fd, EVIOCGABS(ismt ? ABS_MT_POSITION_X : ABS_X), &ix);
        ioctl(fd, EVIOCGABS(ismt ? ABS_MT_POSITION_Y : ABS_Y), &iy);
        if (ix.maximum <= ix.minimum || iy.maximum <= iy.minimum) { close(fd); continue; }
        ax_min = ix.minimum; ax_max = ix.maximum; ay_min = iy.minimum; ay_max = iy.maximum; mt = ismt;
        char nm[64] = "?";
        ioctl(fd, EVIOCGNAME(sizeof nm), nm);
        snprintf(tname, sizeof tname, "%s (%s)", nm, path);
        touch_fd = fd;
    }
    /* The touch sensor reports in the panel's native orientation, like the screen. */
    trot = tr && *tr ? ((atoi(tr) % 360) + 360) % 360 : gfx_rotation();
    return touch_fd < 0 ? -1 : 0;
}

/* raw sensor position -> canvas */
static void map_touch(int rx, int ry, int *cx, int *cy) {
    int pw, ph;
    gfx_panel_size(&pw, &ph);
    double nx = (double)(rx - ax_min) / (ax_max - ax_min), ny = (double)(ry - ay_min) / (ay_max - ay_min);
    int lw = (trot == 90 || trot == 270) ? CH : CW, lh = (trot == 90 || trot == 270) ? CW : CH;
    int lx = (int)(nx * (lw - 1)), ly = (int)(ny * (lh - 1));
    switch (trot) {
    case 90: *cx = ly; *cy = CH - 1 - lx; break;
    case 180: *cx = CW - 1 - lx; *cy = CH - 1 - ly; break;
    case 270: *cx = CW - 1 - ly; *cy = lx; break;
    default: *cx = lx; *cy = ly;
    }
}

int touch_poll(void) {
    if (touch_fd < 0) return 0;
    int changed = 0;
    if (emu()) {
        int n = (int)read(touch_fd, ebuf + elen, sizeof ebuf - 1 - (size_t)elen);
        if (n <= 0) return 0;
        elen += n;
        ebuf[elen] = 0;
        char *nl;
        while ((nl = strchr(ebuf, '\n'))) {
            *nl = 0;
            char cmd[16]; double x, y;
            if (sscanf(ebuf, "%15s %lf %lf", cmd, &x, &y) == 3) {
                if (!strcmp(cmd, "down") || !strcmp(cmd, "move")) { touches[0].down = 1; touches[0].x = (int)x; touches[0].y = (int)y; changed = 1; }
                else if (!strcmp(cmd, "up")) { touches[0].down = 0; touches[0].x = (int)x; touches[0].y = (int)y; changed = 1; }
            }
            memmove(ebuf, nl + 1, (size_t)elen - (size_t)(nl + 1 - ebuf) + 1);
            elen -= (int)(nl + 1 - ebuf);
        }
        if (elen >= (int)sizeof ebuf - 2) elen = 0;
        return changed;
    }
    struct input_event ev[64];
    int n;
    while ((n = (int)read(touch_fd, ev, sizeof ev)) > 0) {
        for (int i = 0; i < n / (int)sizeof ev[0]; i++) {
            struct input_event *e = &ev[i];
            if (e->type == EV_ABS) {
                if (mt) {
                    if (e->code == ABS_MT_SLOT) slot = e->value >= 0 && e->value < MAXT ? e->value : 0;
                    else if (e->code == ABS_MT_TRACKING_ID) pend[slot].down = e->value >= 0;
                    else if (e->code == ABS_MT_POSITION_X) pend[slot].x = e->value;
                    else if (e->code == ABS_MT_POSITION_Y) pend[slot].y = e->value;
                } else {
                    if (e->code == ABS_X) pend[0].x = e->value;
                    else if (e->code == ABS_Y) pend[0].y = e->value;
                }
            } else if (e->type == EV_KEY && e->code == BTN_TOUCH && !mt) {
                pend[0].down = e->value != 0;
            } else if (e->type == EV_SYN && e->code == SYN_REPORT) {
                for (int s = 0; s < MAXT; s++) {
                    touches[s].down = pend[s].down;
                    if (pend[s].down) map_touch(pend[s].x, pend[s].y, &touches[s].x, &touches[s].y);
                }
                changed = 1;
            }
        }
    }
    return changed;
}

/* ================================================================ MIDI */
int midi_fd = -1;
static int midi_out_fd = -1, emu_midi_keep = -1;
static char mname[128] = "none";
static uint8_t rs, mb[3];
static int mneed, mhave, insysex;
static char lbuf[1024];
static int llen;

const char *midi_name(void) { return mname; }

/* The control surface's card: the by-path link the deck's scripts use, else the card named
 * "Control Surface" in /proc/asound/cards. */
static int find_card(void) {
    char link[256];
    ssize_t n = readlink("/dev/snd/by-path/platform-ff190000.serial", link, sizeof link - 1);
    if (n > 0) {
        link[n] = 0;
        char *c = strstr(link, "controlC");
        if (c) return atoi(c + 8);
    }
    FILE *f = fopen("/proc/asound/cards", "r");
    if (!f) return -1;
    char line[256];
    int card = -1;
    while (fgets(line, sizeof line, f))
        if (strstr(line, "Control Surface") || strstr(line, "control-surface")) { card = atoi(line); break; }
    fclose(f);
    return card;
}

int midi_open(void) {
    if (emu()) {
        /* The emulator panel writes the deck's MIDI to midi-in as hex lines; what the deck is sent
         * goes to midi-out, which the panel follows for LEDs and displays. */
        const char *in = "/tmp/emu/midi-in", *outp = "/tmp/emu/midi-out";
        midi_fd = open(in, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        emu_midi_keep = open(in, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        midi_out_fd = open(outp, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
        snprintf(mname, sizeof mname, "emulator panel");
        return midi_fd < 0 ? -1 : 0;
    }
    const char *dev = getenv("DIAG_MIDI");
    char path[64];
    if (!dev || !*dev) {
        int card = find_card();
        if (card < 0) card = 0;
        snprintf(path, sizeof path, "/dev/snd/midiC%dD0", card);
        dev = path;
    }
    midi_fd = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (midi_fd < 0) { snprintf(mname, sizeof mname, "%s: %s", dev, strerror(errno)); return -1; }
    midi_out_fd = midi_fd;
    snprintf(mname, sizeof mname, "%s", dev);
    return 0;
}

static int parse_byte(uint8_t b, midi_msg_t *m) {
    if (b >= 0xF8) return 0;                       /* real-time */
    if (b == 0xF0) { insysex = 1; return 0; }
    if (b == 0xF7) { insysex = 0; return 0; }
    if (b & 0x80) {
        insysex = 0;
        if (b >= 0xF0) { rs = 0; return 0; }
        rs = b; mhave = 0;
        mneed = ((b & 0xF0) == 0xC0 || (b & 0xF0) == 0xD0) ? 1 : 2;
        return 0;
    }
    if (insysex || !rs) return 0;
    mb[mhave++] = b;
    if (mhave < mneed) return 0;
    m->st = rs; m->d1 = mb[0]; m->d2 = mneed > 1 ? mb[1] : 0;
    mhave = 0;
    return 1;
}

int midi_poll(midi_msg_t *m, int max) {
    if (midi_fd < 0) return 0;
    int got = 0;
    if (emu()) {
        int n = (int)read(midi_fd, lbuf + llen, sizeof lbuf - 1 - (size_t)llen);
        if (n > 0) llen += n;
        lbuf[llen] = 0;
        char *nl;
        while (got < max && (nl = strchr(lbuf, '\n'))) {
            *nl = 0;
            char *p = lbuf, *e;
            for (long v; (v = strtol(p, &e, 16)), e != p; p = e)
                if (got < max && parse_byte((uint8_t)v, &m[got])) got++;
            int used = (int)(nl + 1 - lbuf);
            memmove(lbuf, nl + 1, (size_t)(llen - used) + 1);
            llen -= used;
        }
        if (llen >= (int)sizeof lbuf - 2) llen = 0;
        return got;
    }
    uint8_t buf[256];
    int n;
    while (got < max && (n = (int)read(midi_fd, buf, sizeof buf)) > 0)
        for (int i = 0; i < n && got < max; i++)
            if (parse_byte(buf[i], &m[got])) got++;
    return got;
}

void midi_send(const uint8_t *b, int n) {
    if (midi_out_fd < 0 || n <= 0) return;
    if (emu()) {
        char line[1024];
        int k = 0;
        for (int i = 0; i < n && k < (int)sizeof line - 4; i++) k += snprintf(line + k, sizeof line - (size_t)k, i ? " %02x" : "%02x", b[i]);
        line[k++] = '\n';
        if (write(midi_out_fd, line, (size_t)k) < 0) {}
        return;
    }
    while (n > 0) {
        ssize_t w = write(midi_out_fd, b, (size_t)n);
        if (w < 0 && errno == EAGAIN) { usleep(1000); continue; }
        if (w <= 0) return;
        b += w; n -= (int)w;
    }
}
void midi_note(int ch, int note, int vel) { uint8_t b[3] = {(uint8_t)(0x90 | (ch & 15)), (uint8_t)(note & 127), (uint8_t)(vel & 127)}; midi_send(b, 3); }
void midi_cc(int ch, int cc, int val) { uint8_t b[3] = {(uint8_t)(0xB0 | (ch & 15)), (uint8_t)(cc & 127), (uint8_t)(val & 127)}; midi_send(b, 3); }
void midi_sysex_hex(const char *hex) {
    uint8_t b[256];
    int n = 0;
    char *e;
    for (const char *p = hex; n < 256; p = e) {
        long v = strtol(p, &e, 16);
        if (e == p) break;
        b[n++] = (uint8_t)v;
    }
    midi_send(b, n);
}

/* ================================================================ audio */
int audio_out_ok, audio_in_ok;
static char aname[160] = "none";
static snd_pcm_t *pout, *pin;
static unsigned out_ch = 8, in_ch = 2, rate = 44100;
static volatile int tone_mask, run_audio;
static volatile double tone_hz = 1000;
static volatile double tone_db = -24;   /* tone level in dBFS, set from the Audio page */
static volatile double in_db[16];
static pthread_t th_out, th_in;

const char *audio_name(void) { return aname; }
void audio_tone(int mask, double hz) { tone_hz = hz; tone_mask = mask; }
int audio_tone_mask(void) { return tone_mask; }
double audio_tone_hz(void) { return tone_hz; }
void audio_set_level(double db) { tone_db = db < -60 ? -60 : (db > 0 ? 0 : db); }
double audio_level(void) { return tone_db; }
int audio_in_channels(void) { return audio_in_ok ? (int)in_ch : 0; }
double audio_in_level(int ch) { return ch >= 0 && ch < 16 ? in_db[ch] : -100; }

static int setup(snd_pcm_t *p, unsigned *ch, unsigned want_ch) {
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    if (snd_pcm_hw_params_any(p, hw) < 0) return -1;
    if (snd_pcm_hw_params_set_access(p, hw, SND_PCM_ACCESS_RW_INTERLEAVED) < 0) return -1;
    if (snd_pcm_hw_params_set_format(p, hw, SND_PCM_FORMAT_S32_LE) < 0) return -1;
    unsigned maxc = want_ch;
    if (snd_pcm_hw_params_get_channels_max(hw, &maxc) < 0 || maxc > 16) maxc = want_ch;
    *ch = maxc < want_ch ? maxc : want_ch;
    if (snd_pcm_hw_params_set_channels(p, hw, *ch) < 0) return -1;
    unsigned r = rate;
    if (snd_pcm_hw_params_set_rate_near(p, hw, &r, NULL) < 0) return -1;
    rate = r;
    snd_pcm_uframes_t period = 512, buffer = 2048;
    snd_pcm_hw_params_set_period_size_near(p, hw, &period, NULL);
    snd_pcm_hw_params_set_buffer_size_near(p, hw, &buffer);
    if (snd_pcm_hw_params(p, hw) < 0) return -1;
    return snd_pcm_prepare(p);
}

static void *out_thread(void *a) {
    (void)a;
    enum { F = 512 };
    static int32_t buf[F * 16];
    double ph = 0, sweep = 0, gain = 0;
    while (run_audio) {
        const double want = pow(10, tone_db / 20);
        int mask = tone_mask;
        double hz = tone_hz;
        for (int i = 0; i < F; i++) {
            double f = hz > 0 ? hz : 60.0 * pow(2, sweep);   /* sweep: 60 Hz to ~15 kHz over 8 s */
            if (hz <= 0) { sweep += 8.0 / (8.0 * rate); if (sweep > 8) sweep = 0; }
            ph += 2 * M_PI * f / rate;
            if (ph > 2 * M_PI) ph -= 2 * M_PI;
            gain += (want - gain) * 0.002;   /* level changes glide over ~10 ms, no clicks */
            int32_t v = (int32_t)(sin(ph) * gain * 2147483647.0);
            for (unsigned c = 0; c < out_ch; c++) buf[i * out_ch + c] = (mask >> c) & 1 ? v : 0;
        }
        snd_pcm_sframes_t w = snd_pcm_writei(pout, buf, F);
        if (w < 0) { snd_pcm_prepare(pout); usleep(2000); }
    }
    return NULL;
}

static void *in_thread(void *a) {
    (void)a;
    enum { F = 2205 };   /* 50 ms */
    static int32_t buf[F * 16];
    while (run_audio) {
        snd_pcm_sframes_t r = snd_pcm_readi(pin, buf, F);
        if (r < 0) { snd_pcm_prepare(pin); usleep(5000); continue; }
        for (unsigned c = 0; c < in_ch && c < 16; c++) {
            double peak = 0;
            for (int i = 0; i < r; i++) { double v = fabs(buf[i * in_ch + c] / 2147483648.0); if (v > peak) peak = v; }
            in_db[c] = peak > 1e-5 ? 20 * log10(peak) : -100;
        }
    }
    return NULL;
}

int audio_open(void) {
    const char *dev = getenv("DIAG_PCM");
    const char *tries[] = {dev && *dev ? dev : "hw:JP21", "hw:1,0", "plughw:JP21"};
    for (int i = 0; i < 3 && !pout; i++) {
        if (snd_pcm_open(&pout, tries[i], SND_PCM_STREAM_PLAYBACK, 0) < 0) { pout = NULL; continue; }
        if (setup(pout, &out_ch, 8)) { snd_pcm_close(pout); pout = NULL; continue; }
        snprintf(aname, sizeof aname, "%s, %u out", tries[i], out_ch);
        if (snd_pcm_open(&pin, tries[i], SND_PCM_STREAM_CAPTURE, 0) == 0 && setup(pin, &in_ch, 16) == 0) {
            audio_in_ok = 1;
            snprintf(aname + strlen(aname), sizeof aname - strlen(aname), ", %u in", in_ch);
        } else if (pin) { snd_pcm_close(pin); pin = NULL; }
    }
    if (!pout) return -1;
    audio_out_ok = 1;
    for (int c = 0; c < 16; c++) in_db[c] = -100;
    run_audio = 1;
    pthread_create(&th_out, NULL, out_thread, NULL);
    if (audio_in_ok) pthread_create(&th_in, NULL, in_thread, NULL);
    return 0;
}

void audio_close(void) {
    if (!run_audio) return;
    tone_mask = 0;
    run_audio = 0;
    pthread_join(th_out, NULL);
    if (audio_in_ok) pthread_join(th_in, NULL);
    snd_pcm_drop(pout); snd_pcm_close(pout);
    if (pin) { snd_pcm_drop(pin); snd_pcm_close(pin); }
}
