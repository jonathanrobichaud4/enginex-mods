/* sc-diagnostics: hardware diagnostics for the Denon DJ SC LIVE 4 (Engine OS 5.3, EngineX).
 *
 * A stand-in for the factory test application, which only runs inMusic-signed images. It runs
 * while Engine is stopped and takes the screen, touch, control surface and audio for itself:
 *   CONTROLS  every button, pad, knob, fader, encoder, jog wheel and switch, drawn where it is on
 *             the deck, lights up as it's used; untouched ones stay grey
 *   LIGHTS    all LEDs on/off, a chase through every LED by name, pad colours, VU meters and
 *             the jog wheel displays
 *   AUDIO     a test tone to any of the codec's outputs (Main, Booth, Phones), input meters
 *   SCREEN    full-screen colours and patterns for dead pixels, banding and uniformity
 *   TOUCH     a grid that fills where it's touched, with every finger tracked
 * Exit with EXIT, or hold the BROWSE knob down for 3 seconds.
 *
 *   sc-diagnostics            run the app (Engine must be stopped)
 *   sc-diagnostics --serve    the launcher: a page on port 8092 (DIAG_PORT) and POST /start,
 *                             which stops Engine, runs the app and starts Engine again
 * The control map comes from Engine's own JP21 assignment file, by way of the SC LIVE 4
 * emulator's panel.json (assets.h, gen_assets.py). */
#define _GNU_SOURCE
#include "assets.h"
#include "gfx.h"
#include "io.h"
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define VERSION "1.1.0"
enum { T_BTN, T_PAD, T_KNOB, T_FADER, T_XFADER, T_ENC, T_JOG, T_PITCH, T_SW, T_SEL, T_VU };

static const uint32_t BG = 0xFF0D0F12, PANEL = 0xFF16191D, IDLE = 0xFF2B2F35, LINE_C = 0xFF3A3F46,
                      TXT = 0xFFE6E8EB, DIM = 0xFF8A9099, ACCENT = 0xFF4AA3FF, OK = 0xFF2E9E5B, WARN = 0xFFE0A030;

static volatile sig_atomic_t quit;
static void on_term(int s) { (void)s; quit = 1; }

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

/* ================================================================ control state */
typedef struct { int seen, active, value, count; double flash; } cstate_t;
static cstate_t cs[NCTLS];
static int16_t note_map[16][128], cc_map[16][128], cc2_map[16][128];
static char last_msg[160] = "Use any control on the deck.";

static int is_input(int t) { return t != T_VU; }

static void build_maps(void) {
    for (int c = 0; c < 16; c++)
        for (int k = 0; k < 128; k++) note_map[c][k] = cc_map[c][k] = cc2_map[c][k] = -1;
    for (int i = 0; i < NCTLS; i++) {
        const ctl_t *c = &CTLS[i];
        if (c->ch > 15) continue;
        if (c->n >= 0 && c->n < 128 && (c->type == T_BTN || c->type == T_PAD || c->type == T_ENC || c->type == T_JOG ||
                                        c->type == T_SW || c->type == T_SEL)) note_map[c->ch][c->n] = (int16_t)i;
        if (c->cc >= 0 && c->cc < 128 && c->type != T_VU) cc_map[c->ch][c->cc] = (int16_t)i;
        if (c->cc2 >= 0 && c->cc2 < 128) cc2_map[c->ch][c->cc2] = (int16_t)i;
        cs[i].value = (c->type == T_PITCH || c->type == T_JOG) ? 8192 : 0;
    }
}

static int seen_count(int *total) {
    int n = 0, t = 0;
    for (int i = 0; i < NCTLS; i++) if (is_input(CTLS[i].type)) { t++; n += cs[i].seen; }
    *total = t;
    return n;
}

static double hold_browse = 0;
/* At start the surface reports every knob and fader's position; that sets the values but
 * doesn't count as the control having been tried. */
static double settle_until;
static void handle_midi(const midi_msg_t *m) {
    int st = m->st & 0xF0, ch = m->st & 15, i = -1;
    double t = now_s();
    if (st == 0x90 || st == 0x80) {
        i = note_map[ch][m->d1];
        int on = st == 0x90 && m->d2 > 0;
        if (i >= 0) {
            const ctl_t *c = &CTLS[i];
            if (c->type == T_SW || c->type == T_SEL) cs[i].value = m->d2;
            else cs[i].active = on;
            /* BROWSE held down for 3 s leaves the app, for when touch doesn't work */
            if (!strcmp(c->id, "browse")) hold_browse = on ? t : 0;
        }
        snprintf(last_msg, sizeof last_msg, "Ch %d note %d %s (vel %d)%s%s", ch + 1, m->d1, on ? "on" : "off", m->d2,
                 i >= 0 ? "  =  " : "  (not in the map)", i >= 0 ? CTLS[i].id : "");
    } else if (st == 0xB0) {
        int msb = 1;
        i = cc_map[ch][m->d1];
        if (i < 0) { i = cc2_map[ch][m->d1]; msb = 0; }
        if (i >= 0) {
            const ctl_t *c = &CTLS[i];
            if (c->type == T_ENC) cs[i].count += m->d2 < 64 ? m->d2 : m->d2 - 128;
            else if (c->type == T_JOG || c->type == T_PITCH) {
                cs[i].value = msb ? (m->d2 << 7) | (cs[i].value & 127) : (cs[i].value & ~127) | m->d2;
            } else cs[i].value = m->d2;
        }
        snprintf(last_msg, sizeof last_msg, "Ch %d CC %d = %d%s%s", ch + 1, m->d1, m->d2,
                 i >= 0 ? "  =  " : "  (not in the map)", i >= 0 ? CTLS[i].id : "");
    } else {
        snprintf(last_msg, sizeof last_msg, "Message %02X %02X %02X", m->st, m->d1, m->d2);
    }
    if (i >= 0 && t >= settle_until) { cs[i].seen = 1; cs[i].flash = t; }
}

/* ================================================================ UI */
enum { P_CONTROLS, P_LIGHTS, P_AUDIO, P_SCREEN, P_TOUCH, P_EXIT, NPAGES };
static const char *TABS[NPAGES] = {"CONTROLS", "LIGHTS", "AUDIO", "SCREEN", "TOUCH", "EXIT"};
static int page = P_CONTROLS;
enum { TAB_H = 56 };

typedef struct { int x, y, w, h; int id; } hit_t;
static hit_t hits[64];
static int nhits;
static void add_hit(int x, int y, int w, int h, int id) { if (nhits < 64) hits[nhits++] = (hit_t){x, y, w, h, id}; }

static void button(int x, int y, int w, int h, const char *lab, int on, int id) {
    fill_round(x, y, w, h, 8, on ? ACCENT : IDLE);
    text_c(x + w / 2, y + (h - text_h(FM)) / 2, FM, on ? 0xFF06121F : TXT, lab);
    add_hit(x, y, w, h, id);
}

static void tabs(void) {
    fill(0, 0, CW, TAB_H, PANEL);
    int w = CW / NPAGES;
    for (int i = 0; i < NPAGES; i++) {
        int on = i == page;
        if (on) fill(i * w, TAB_H - 4, w, 4, ACCENT);
        text_c(i * w + w / 2, (TAB_H - text_h(FM)) / 2, FM, on ? TXT : (i == P_EXIT ? WARN : DIM), TABS[i]);
        add_hit(i * w, 0, w, TAB_H, 1000 + i);
    }
}

/* ---- CONTROLS: the deck's layout, scaled to the screen ---- */
static double sc, ox, oy;
static int X(int v) { return (int)(ox + v * sc); }
static int Y(int v) { return (int)(oy + v * sc); }
static int S(int v) { int r = (int)(v * sc); return r < 1 ? 1 : r; }

static void draw_controls(void) {
    double t = now_s();
    sc = fmin(CW / (double)PANEL_W, (CH - TAB_H - 40) / (double)PANEL_H);
    ox = (CW - PANEL_W * sc) / 2;
    oy = TAB_H + 4;
    for (int i = 0; i < NDECOS; i++) {
        const deco_t *d = &DECOS[i];
        if (d->lab[0]) text_c(X(d->x + d->w / 2), Y(d->y), FS, DIM, d->lab);
        else frame(X(d->x), Y(d->y), S(d->w), S(d->h), 1, LINE_C);
    }
    for (int i = 0; i < NCTLS; i++) {
        const ctl_t *c = &CTLS[i];
        cstate_t *s = &cs[i];
        int x = X(c->x), y = Y(c->y), w = S(c->w), h = S(c->h);
        double fl = 1 - fmin(1, (t - s->flash) / 0.6);
        uint32_t base = s->seen ? mixc(IDLE, OK, 0.75) : IDLE;
        uint32_t col = s->active ? ACCENT : mixc(base, ACCENT, fl);
        switch (c->type) {
        case T_BTN: case T_PAD: case T_SW: case T_SEL:
            if (c->round) fill_circle(x + w / 2, y + h / 2, w / 2, col);
            else fill_round(x, y, w, h, 3, col);
            if (c->type == T_SW || c->type == T_SEL) {
                char v[8]; snprintf(v, sizeof v, "%d", s->value);
                text_c(x + w / 2, y + h / 2 - 6, FS, TXT, s->seen ? v : "");
            }
            break;
        case T_KNOB:
            fill_circle(x + w / 2, y + h / 2, w / 2, mixc(IDLE, BG, 0.3));
            arc(x + w / 2, y + h / 2, w / 2, 3, -2.4, -2.4 + 4.8 * s->value / 127.0, col);
            break;
        case T_ENC: {
            fill_circle(x + w / 2, y + h / 2, w / 2, col);
            char v[12]; snprintf(v, sizeof v, "%d", s->count);
            if (s->seen) text_c(x + w / 2, y + h / 2 - 7, FS, TXT, v);
            break; }
        case T_FADER: case T_PITCH: {
            fill(x + w / 2 - 2, y, 4, h, LINE_C);
            double v = c->type == T_PITCH ? s->value / 16383.0 : 1 - s->value / 127.0;
            fill_round(x, y + (int)(v * (h - 10)), w, 10, 2, s->seen ? (fl > 0 ? ACCENT : OK) : DIM);
            break; }
        case T_XFADER: {
            fill(x, y + h / 2 - 2, w, 4, LINE_C);
            double v = s->value / 127.0;
            fill_round(x + (int)(v * (w - 12)), y, 12, h, 2, s->seen ? (fl > 0 ? ACCENT : OK) : DIM);
            break; }
        case T_JOG: {
            int r = w / 2, cx = x + r, cy = y + r;
            ring(cx, cy, r, 3, s->active ? ACCENT : (s->seen ? OK : LINE_C));
            fill_circle(cx, cy, r - 6, mixc(IDLE, BG, 0.4));
            double a = s->value / 16384.0 * 2 * M_PI;
            line(cx, cy, cx + (int)(sin(a) * (r - 8)), cy - (int)(cos(a) * (r - 8)), 3, s->seen ? col : DIM);
            text_c(cx, cy + r / 3, FS, DIM, c->lab);
            break; }
        case T_VU:
            fill(x, y, w, h, mixc(IDLE, BG, 0.5));
            break;
        }
    }
    int total, n = seen_count(&total);
    char b[200];
    snprintf(b, sizeof b, "Seen %d of %d controls", n, total);
    fill(0, CH - 34, CW, 34, PANEL);
    text(16, CH - 30, FM, n == total ? OK : TXT, b);
    text(360, CH - 27, FS, DIM, last_msg);
    button(CW - 130, CH - 32, 120, 30, "RESET", 0, 1);
}

/* ---- LIGHTS ---- */
static int light_mode;      /* 0 none, 1 all on, 2 chase, 3..7 pad colours, 8 VU, 9 jog displays */
static int chase_i = -1;
static double chase_t;
static char light_note[160] = "Lights are as Engine left them.";

static void leds_all(int on) {
    for (int i = 0; i < NCTLS; i++) {
        const ctl_t *c = &CTLS[i];
        if (c->type == T_BTN || c->type == T_ENC) { if (c->n >= 0) midi_note(c->ch, c->n, on ? 127 : 0); }
        else if (c->type == T_PAD) midi_note(c->ch, c->n, on ? 0x3F : 0);
        else if (c->type == T_PITCH && c->extra >= 0) midi_note(c->ch, c->extra, on ? 127 : 0);
        else if (c->type == T_VU) midi_cc(c->ch, c->cc, on ? 127 : 0);
    }
}

/* The jog wheel displays (Engine's LcdWheelDisplay): CC 0x30 ring, 0x31-0x3F parts, note 0x38
 * the deck number, note 0x3D every pixel; time and BPM by SysEx. */
static void jog_lcd(int on) {
    for (int ch = 4; ch <= 5; ch++) {
        for (int cc = 0x31; cc <= 0x3F; cc++) midi_cc(ch, cc, on ? 127 : 0);
        midi_cc(ch, 0x30, on ? 0x40 : 0);
        midi_note(ch, 0x38, on ? ch - 3 : 0);
        midi_note(ch, 0x3D, on ? 127 : 0);
        if (on) {
            char sx[96];
            snprintf(sx, sizeof sx, "F0 00 02 0B 0%d 01 08 3B 3B 63 F7", ch);       /* -8:59:59.9 */
            midi_sysex_hex(sx);
            snprintf(sx, sizeof sx, "F0 00 02 0B 0%d 02 00 01 05 0B 03 08 F7", ch); /* 888.88 BPM */
            midi_sysex_hex(sx);
        }
    }
}

static void pads(int r, int g, int b, int rainbow) {
    int k = 0;
    for (int i = 0; i < NCTLS; i++) {
        const ctl_t *c = &CTLS[i];
        if (c->type != T_PAD) continue;
        int v = rainbow ? ((k % 6 == 0) ? 0x30 : (k % 6 == 1) ? 0x3C : (k % 6 == 2) ? 0x0C : (k % 6 == 3) ? 0x0F : (k % 6 == 4) ? 0x03 : 0x33)
                        : (r << 4 | g << 2 | b);
        midi_note(c->ch, c->n, v);
        k++;
    }
}

static int next_led(int from) {
    for (int i = from + 1; i < NCTLS; i++) {
        int t = CTLS[i].type;
        if (t == T_BTN || t == T_PAD || t == T_VU || (t == T_ENC && CTLS[i].n >= 0) || (t == T_PITCH && CTLS[i].extra >= 0)) return i;
    }
    return -1;
}
static void led_set(int i, int on) {
    const ctl_t *c = &CTLS[i];
    if (c->type == T_VU) midi_cc(c->ch, c->cc, on ? 127 : 0);
    else if (c->type == T_PAD) midi_note(c->ch, c->n, on ? 0x3F : 0);
    else if (c->type == T_PITCH) midi_note(c->ch, c->extra, on ? 127 : 0);
    else midi_note(c->ch, c->n, on ? 127 : 0);
}

static void lights_tick(void) {
    double t = now_s();
    if (light_mode == 2 && t - chase_t > 0.35) {
        if (chase_i >= 0) led_set(chase_i, 0);
        chase_i = next_led(chase_i);
        if (chase_i < 0) chase_i = next_led(-1);
        if (chase_i >= 0) {
            led_set(chase_i, 1);
            snprintf(light_note, sizeof light_note, "Now lit: %s  (%s)", CTLS[chase_i].id, CTLS[chase_i].lab);
        }
        chase_t = t;
    } else if (light_mode == 8 && t - chase_t > 0.05) {      /* VU meters sweep up and down */
        int v = (int)(fabs(fmod(t, 2.0) - 1.0) * 127);
        for (int i = 0; i < NCTLS; i++) if (CTLS[i].type == T_VU) midi_cc(CTLS[i].ch, CTLS[i].cc, v);
        chase_t = t;
    } else if (light_mode == 9 && t - chase_t > 0.08) {      /* jog ring goes round */
        static int seg = 1;
        for (int ch = 4; ch <= 5; ch++) midi_cc(ch, 0x30, seg);
        seg = seg % 48 + 1;
        chase_t = t;
    }
}

static void lights_action(int id) {
    if (light_mode == 2 && chase_i >= 0) led_set(chase_i, 0);
    chase_i = -1;
    switch (id) {
    case 10: leds_all(1); jog_lcd(1); light_mode = 1; snprintf(light_note, sizeof light_note, "Every LED, pad and jog display segment should be lit."); break;
    case 11: leds_all(0); jog_lcd(0); light_mode = 0; snprintf(light_note, sizeof light_note, "Everything should be dark."); break;
    case 12: leds_all(0); light_mode = 2; chase_t = 0; break;
    case 13: pads(3, 0, 0, 0); light_mode = 3; snprintf(light_note, sizeof light_note, "All pads red."); break;
    case 14: pads(0, 3, 0, 0); light_mode = 4; snprintf(light_note, sizeof light_note, "All pads green."); break;
    case 15: pads(0, 0, 3, 0); light_mode = 5; snprintf(light_note, sizeof light_note, "All pads blue."); break;
    case 16: pads(3, 3, 3, 0); light_mode = 6; snprintf(light_note, sizeof light_note, "All pads white."); break;
    case 17: pads(0, 0, 0, 1); light_mode = 7; snprintf(light_note, sizeof light_note, "Pads in six colours."); break;
    case 18: light_mode = 8; snprintf(light_note, sizeof light_note, "Channel and master meters sweep up and down."); break;
    case 19: jog_lcd(1); light_mode = 9; snprintf(light_note, sizeof light_note, "Jog displays: every part on, the ring going round, -8:59:59.9 and 888.88 BPM."); break;
    }
}

static void draw_lights(void) {
    static const char *labs[] = {"ALL ON", "ALL OFF", "CHASE", "PADS RED", "PADS GREEN", "PADS BLUE", "PADS WHITE", "PADS MIXED", "METERS", "JOG DISPLAYS"};
    static const int modes[] = {1, 0, 2, 3, 4, 5, 6, 7, 8, 9};
    text(40, TAB_H + 30, FL, TXT, "Lights");
    text(40, TAB_H + 80, FM, DIM, "Pick a test and check the deck. CHASE lights one LED at a time and names it here.");
    for (int i = 0; i < 10; i++) {
        int col = i % 5, row = i / 5;
        button(40 + col * 242, TAB_H + 140 + row * 110, 222, 86, labs[i], light_mode == modes[i] && !(i == 1 && light_mode != 0), 10 + i);
    }
    fill_round(40, TAB_H + 380, CW - 80, 70, 10, PANEL);
    text(64, TAB_H + 400, FM, TXT, light_note);
}

/* ---- AUDIO ---- */
enum { NLEVELS = 5 };
static const double LEVELS[NLEVELS] = {-36, -24, -18, -12, -6};
static void audio_action(int id) {
    static const int masks[] = {0x01, 0x02, 0x03, 0x04, 0x08, 0x0C, 0x10, 0x20, 0x30, 0x40, 0x80, 0xFF};
    static const double hzs[] = {100, 440, 1000, 5000, 0};
    if (id >= 30 && id < 42) audio_tone(audio_tone_mask() == masks[id - 30] ? 0 : masks[id - 30], audio_tone_hz());
    else if (id == 42) audio_tone(0, audio_tone_hz());
    else if (id >= 50 && id < 55) audio_tone(audio_tone_mask(), hzs[id - 50]);
    else if (id == 80) audio_set_level(audio_level() - 3);
    else if (id == 81) audio_set_level(audio_level() + 3);
    else if (id >= 82 && id < 82 + NLEVELS) audio_set_level(LEVELS[id - 82]);
}

static void draw_audio(void) {
    static const char *labs[] = {"MAIN L", "MAIN R", "MAIN L+R", "BOOTH L", "BOOTH R", "BOOTH L+R", "PHONES L", "PHONES R", "PHONES L+R", "OUT 7", "OUT 8", "ALL"};
    static const int masks[] = {0x01, 0x02, 0x03, 0x04, 0x08, 0x0C, 0x10, 0x20, 0x30, 0x40, 0x80, 0xFF};
    text(40, TAB_H + 24, FL, TXT, "Audio");
    if (!audio_out_ok) {
        text(40, TAB_H + 80, FM, WARN, "No audio device. The codec may still be in use; check that Engine is stopped.");
        return;
    }
    text(40, TAB_H + 74, FS, DIM, "Start low. Tones go straight into the codec, so the deck's volume knobs don't change them; set the level here.");
    char b[200];
    snprintf(b, sizeof b, "Device: %s", audio_name());
    text(40, TAB_H + 94, FS, DIM, b);
    int m = audio_tone_mask();
    for (int i = 0; i < 12; i++) {
        int col = i % 6, row = i / 6;
        button(40 + col * 202, TAB_H + 124 + row * 74, 186, 60, labs[i], m == masks[i], 30 + i);
    }
    button(40, TAB_H + 280, 186, 56, "STOP", 0, 42);
    static const char *hz[] = {"100 Hz", "440 Hz", "1 kHz", "5 kHz", "SWEEP"};
    static const double hv[] = {100, 440, 1000, 5000, 0};
    for (int i = 0; i < 5; i++) button(242 + i * 202, TAB_H + 280, 186, 56, hz[i], fabs(audio_tone_hz() - hv[i]) < 0.5, 50 + i);
    /* level: - / readout / + and quick presets */
    int ly = TAB_H + 350;
    double lv = audio_level();
    button(40, ly, 86, 56, "-", 0, 80);
    fill_round(134, ly, 150, 56, 8, PANEL);
    snprintf(b, sizeof b, "%.0f dB", lv);
    text_c(134 + 75, ly + 16, FM, TXT, b);
    button(292, ly, 86, 56, "+", 0, 81);
    for (int i = 0; i < NLEVELS; i++) {
        snprintf(b, sizeof b, "%.0f dB", LEVELS[i]);
        button(402 + i * 172, ly, 158, 56, b, fabs(lv - LEVELS[i]) < 0.5, 82 + i);
    }
    text(40, TAB_H + 430, FM, TXT, "Inputs");
    int n = audio_in_channels();
    if (!n) { text(150, TAB_H + 432, FS, DIM, "no capture device"); return; }
    text(150, TAB_H + 434, FS, DIM, "Plug a source into the mic and aux inputs; each input channel shows its peak level.");
    for (int c = 0; c < n && c < 8; c++) {
        double db = audio_in_level(c);
        int y = TAB_H + 466 + c * 38, w = CW - 260;
        snprintf(b, sizeof b, "IN %d", c + 1);
        text(40, y + 4, FM, TXT, b);
        fill_round(120, y, w, 26, 6, PANEL);
        double f = db <= -60 ? 0 : (db + 60) / 60;
        fill_round(120, y, (int)(w * f), 26, 6, db > -3 ? 0xFFE04040 : (db > -12 ? WARN : OK));
        snprintf(b, sizeof b, db <= -99 ? "silent" : "%.1f dB", db);
        text(130 + w, y + 4, FM, DIM, b);
    }
}

/* ---- SCREEN ---- */
enum { NPAT = 8 };
static const char *PATS[NPAT] = {"RED", "GREEN", "BLUE", "WHITE", "BLACK", "GREY RAMP", "COLOUR BARS", "CHECKER"};
static int pattern = -1;
static void draw_pattern(int p) {
    static const uint32_t solid[] = {0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFFFF, 0xFF000000};
    if (p < 5) { fill(0, 0, CW, CH, solid[p]); return; }
    if (p == 5) { for (int x = 0; x < CW; x++) { int v = x * 255 / (CW - 1); fill(x, 0, 1, CH / 2, rgb(v, v, v)); fill(x, CH / 2, 1, CH / 2, rgb(v, 0, 0) | ((uint32_t)(v >> 1) << 8)); } return; }
    if (p == 6) {
        static const uint32_t bars[] = {0xFFFFFFFF, 0xFFFFFF00, 0xFF00FFFF, 0xFF00FF00, 0xFFFF00FF, 0xFFFF0000, 0xFF0000FF, 0xFF000000};
        for (int i = 0; i < 8; i++) fill(i * CW / 8, 0, CW / 8 + 1, CH, bars[i]);
        return;
    }
    if (p == 7) { for (int y = 0; y < CH; y++) for (int x = 0; x < CW; x++) canvas[y * CW + x] = ((x ^ y) & 1) ? 0xFFFFFFFF : 0xFF000000; }
}
static void draw_screen_page(void) {
    text(40, TAB_H + 24, FL, TXT, "Screen");
    text(40, TAB_H + 74, FM, DIM, "Pick a pattern; it fills the screen. Tap to go to the next one, and after the last you're back here.");
    char b[160];
    int pw, ph;
    gfx_panel_size(&pw, &ph);
    snprintf(b, sizeof b, "Panel %dx%d through %s, drawn turned %d degrees", pw, ph, gfx_output_name(), gfx_rotation());
    text(40, TAB_H + 104, FS, DIM, b);
    for (int i = 0; i < NPAT; i++) button(40 + (i % 3) * 404, TAB_H + 140 + (i / 3) * 110, 384, 90, PATS[i], 0, 60 + i);
}

/* ---- TOUCH ---- */
enum { GW = 32, GH = 19 };
static uint8_t grid[GH][GW];
static int tx_prev[MAXT], ty_prev[MAXT], tdown_prev[MAXT];
static uint32_t trail_col[MAXT] = {0xFF4AA3FF, 0xFFFF7A45, 0xFF3DDC84, 0xFFE0C030, 0xFFC86BFA, 0xFF45D6E8, 0xFFFF5C8A, 0xFFB0B0B0, 0xFF8AE04A, 0xFFFFFFFF};
static void draw_touch(void) {
    int top = TAB_H, cw = CW / GW, chh = (CH - top) / GH, n = 0, covered = 0;
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++) {
            covered += grid[y][x];
            fill(x * cw + 1, top + y * chh + 1, cw - 2, chh - 2, grid[y][x] ? mixc(PANEL, OK, 0.7) : PANEL);
        }
    for (int i = 0; i < MAXT; i++) {
        if (!touches[i].down) continue;
        n++;
        fill_circle(touches[i].x, touches[i].y, 26, trail_col[i]);
        char b[48];
        snprintf(b, sizeof b, "%d: %d, %d", i + 1, touches[i].x, touches[i].y);
        text(touches[i].x + 32, touches[i].y - 30, FM, TXT, b);
    }
    char b[96];
    snprintf(b, sizeof b, "%d finger%s  -  %d%% covered", n, n == 1 ? "" : "s", covered * 100 / (GW * GH));
    fill_round(12, CH - 46, text_w(FM, b) + 24, 36, 8, 0xC0000000);
    text(24, CH - 40, FM, TXT, b);
    button(CW - 140, CH - 50, 128, 40, "CLEAR", 0, 70);
}
static void touch_track(void) {
    int top = TAB_H, cw = CW / GW, chh = (CH - top) / GH;
    for (int i = 0; i < MAXT; i++) {
        if (!touches[i].down) { tdown_prev[i] = 0; continue; }
        int x0 = tdown_prev[i] ? tx_prev[i] : touches[i].x, y0 = tdown_prev[i] ? ty_prev[i] : touches[i].y;
        for (int k = 0; k <= 16; k++) {
            int x = x0 + (touches[i].x - x0) * k / 16, y = y0 + (touches[i].y - y0) * k / 16;
            int gx = x / cw, gy = (y - top) / chh;
            if (gx >= 0 && gx < GW && gy >= 0 && gy < GH) grid[gy][gx] = 1;
        }
        tx_prev[i] = touches[i].x; ty_prev[i] = touches[i].y; tdown_prev[i] = 1;
    }
}

/* ---- frame ---- */
static void draw(void) {
    nhits = 0;
    /* A pattern is drawn and sent to the panel once; redrawing it constantly made the deck's
       panel shimmer on the fine ones. */
    static int shown = -1;
    if (pattern >= 0) {
        add_hit(0, 0, CW, CH, 69);
        if (shown != pattern) { draw_pattern(pattern); gfx_present(); shown = pattern; }
        return;
    }
    shown = -1;
    fill(0, 0, CW, CH, BG);
    switch (page) {
    case P_CONTROLS: draw_controls(); break;
    case P_LIGHTS: draw_lights(); break;
    case P_AUDIO: draw_audio(); break;
    case P_SCREEN: draw_screen_page(); break;
    case P_TOUCH: draw_touch(); break;
    }
    tabs();
    if (page == P_CONTROLS || page == P_LIGHTS) {
        char b[200];
        snprintf(b, sizeof b, "sc-diagnostics %s   MIDI: %s   touch: %s", VERSION, midi_fd >= 0 ? midi_name() : "none", touch_fd >= 0 ? "ok" : "none");
        if (page == P_LIGHTS) text(40, CH - 28, FS, DIM, b);
    }
    gfx_present();
}

static void tap(int x, int y) {
    for (int i = nhits - 1; i >= 0; i--) {
        hit_t *h = &hits[i];
        if (x < h->x || y < h->y || x >= h->x + h->w || y >= h->y + h->h) continue;
        int id = h->id;
        if (id >= 1000) {
            int p = id - 1000;
            if (p == P_EXIT) quit = 1;
            else page = p;
        } else if (id == 1) { for (int k = 0; k < NCTLS; k++) { cs[k].seen = 0; cs[k].count = 0; } snprintf(last_msg, sizeof last_msg, "Cleared."); }
        else if (id >= 10 && id < 20) lights_action(id);
        else if ((id >= 30 && id < 55) || (id >= 80 && id < 90)) audio_action(id);
        else if (id >= 60 && id < 60 + NPAT) pattern = id - 60;
        else if (id == 69) pattern = pattern + 1 < NPAT ? pattern + 1 : -1;
        else if (id == 70) memset(grid, 0, sizeof grid);
        return;
    }
}

static int run_app(void) {
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGPIPE, SIG_IGN);
    if (gfx_open()) return 1;
    build_maps();
    if (touch_open()) fprintf(stderr, "diag: no touchscreen found\n");
    else fprintf(stderr, "diag: touch: %s\n", touch_name());
    if (midi_open()) fprintf(stderr, "diag: control surface: %s\n", midi_name());
    else {
        fprintf(stderr, "diag: control surface: %s\n", midi_name());
        midi_sysex_hex("F0 00 02 0B 7F 12 04 F7"); /* ask the surface for every knob and fader's position */
        settle_until = now_s() + 1.5;
    }
    if (audio_open()) fprintf(stderr, "diag: no audio device\n");
    else fprintf(stderr, "diag: audio: %s\n", audio_name());
    int was_down = 0;
    double last = 0;
    while (!quit) {
        struct pollfd pf[2];
        int n = 0;
        if (touch_fd >= 0) pf[n++] = (struct pollfd){touch_fd, POLLIN, 0};
        if (midi_fd >= 0) pf[n++] = (struct pollfd){midi_fd, POLLIN, 0};
        poll(pf, (nfds_t)n, 30);
        int dirty = 0;
        if (touch_poll()) {
            dirty = 1;
            if (page == P_TOUCH && pattern < 0) touch_track();
            if (touches[0].down && !was_down) tap(touches[0].x, touches[0].y);
            was_down = touches[0].down;
        }
        midi_msg_t m[64];
        int k = midi_poll(m, 64);
        for (int i = 0; i < k; i++) handle_midi(&m[i]);
        if (k) dirty = 1;
        if (hold_browse > 0 && now_s() - hold_browse > 3) quit = 1;
        lights_tick();
        double t = now_s();
        if (dirty || t - last > (page == P_AUDIO ? 0.06 : 0.25)) { draw(); last = t; }
    }
    alarm(4);   /* never leave Engine waiting on a stuck device; SIGALRM ends the process */
    audio_tone(0, audio_tone_hz());
    if (midi_fd >= 0) { leds_all(0); jog_lcd(0); }
    fill(0, 0, CW, CH, 0xFF000000);
    text_c(CW / 2, CH / 2 - 20, FL, TXT, "Starting Engine...");
    gfx_present();
    audio_close();
    gfx_close();
    return 0;
}

/* ================================================================ launcher (--serve) */
static pid_t runner;
static char self_path[512];

static const char PAGE[] =
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>SC LIVE 4 diagnostics</title><style>body{font:16px system-ui,sans-serif;background:#111;color:#eee;max-width:560px;margin:8vh auto;padding:0 20px}"
    "button{font:inherit;font-weight:600;padding:14px 22px;border:0;border-radius:8px;margin:6px 8px 6px 0;cursor:pointer}"
    "#go{background:#2e9e5b;color:#fff}#st{background:#444;color:#fff}p{color:#aaa}</style></head><body>"
    "<h1>SC LIVE 4 diagnostics</h1><p>Starting stops Engine and takes over the screen, controls and audio. "
    "Leave with EXIT on the deck's screen, by holding the BROWSE knob down for 3 seconds, or with Stop here; Engine starts again by itself.</p>"
    "<p id=s>...</p><button id=go>Start diagnostics</button><button id=st>Stop</button>"
    "<script>const s=document.getElementById('s');async function up(){try{const r=await (await fetch('status')).json();"
    "s.textContent=r.running?'Running on the deck.':'Not running. Engine is in charge.'}catch(e){s.textContent='No answer from the deck.'}}"
    "go.onclick=async()=>{await fetch('start',{method:'POST'});setTimeout(up,800)};st.onclick=async()=>{await fetch('stop',{method:'POST'});setTimeout(up,800)};"
    "up();setInterval(up,2000)</script></body></html>";

static int engine_cmd(const char *env, const char *def) {
    const char *c = getenv(env);
    return system(c && *c ? c : def);
}

static void start_run(void) {
    if (runner > 0 && waitpid(runner, NULL, WNOHANG) == 0) return; /* already running */
    runner = fork();
    if (runner != 0) return;
    setsid();
    fprintf(stderr, "diag: stopping Engine\n");
    engine_cmd("DIAG_STOP_ENGINE", "systemctl stop engine.service");
    sleep(1);
    pid_t app = fork();
    if (app == 0) { execl(self_path, self_path, (char *)NULL); _exit(127); }
    FILE *f = fopen("/tmp/sc-diagnostics.pid", "w");
    if (f) { fprintf(f, "%d\n", app); fclose(f); }
    int st = 0;
    waitpid(app, &st, 0);
    unlink("/tmp/sc-diagnostics.pid");
    fprintf(stderr, "diag: app ended (status %d), starting Engine\n", st);
    engine_cmd("DIAG_START_ENGINE", "systemctl start engine.service");
    _exit(0);
}
static int is_running(void) { return runner > 0 && waitpid(runner, NULL, WNOHANG) == 0; }
static void stop_run(void) {
    FILE *f = fopen("/tmp/sc-diagnostics.pid", "r");
    int pid = 0;
    if (f) { if (fscanf(f, "%d", &pid) != 1) pid = 0; fclose(f); }
    if (pid > 0) kill(pid, SIGTERM);
}

static void reply(int fd, const char *status, const char *type, const char *body) {
    char h[256];
    int n = snprintf(h, sizeof h, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                     status, type, strlen(body));
    if (write(fd, h, (size_t)n) < 0 || write(fd, body, strlen(body)) < 0) {}
}

static int serve(void) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_DFL);
    const char *ps = getenv("DIAG_PORT");
    int port = ps && *ps ? atoi(ps) : 8092;
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 8)) { perror("diag: listen"); return 1; }
    fprintf(stderr, "diag: launcher on port %d\n", port);
    for (;;) {
        int c = accept4(s, NULL, NULL, SOCK_CLOEXEC);
        if (c < 0) continue;
        struct timeval tv = {3, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        char req[2048] = {0};
        ssize_t n = read(c, req, sizeof req - 1);
        if (n > 0) {
            char m[8] = "", path[128] = "";
            sscanf(req, "%7s %127s", m, path);
            if (!strcmp(m, "POST") && !strcmp(path, "/start")) { start_run(); reply(c, "200 OK", "application/json", "{\"ok\":true}"); }
            else if (!strcmp(m, "POST") && !strcmp(path, "/stop")) { stop_run(); reply(c, "200 OK", "application/json", "{\"ok\":true}"); }
            else if (!strcmp(path, "/status")) reply(c, "200 OK", "application/json", is_running() ? "{\"running\":true}" : "{\"running\":false}");
            else if (!strcmp(m, "GET") && (!strcmp(path, "/") || !strcmp(path, "/index.html"))) reply(c, "200 OK", "text/html; charset=utf-8", PAGE);
            else reply(c, "404 Not Found", "text/plain", "not found\n");
        }
        close(c);
    }
}

int main(int argc, char **argv) {
    ssize_t n = readlink("/proc/self/exe", self_path, sizeof self_path - 1);
    if (n > 0) self_path[n] = 0; else snprintf(self_path, sizeof self_path, "%s", argv[0]);
    if (argc > 1 && !strcmp(argv[1], "--serve")) return serve();
    /* in the SC LIVE 4 emulator (no real screen, its framebuffer file instead) */
    if (!getenv("EMU_FB") && access("/dev/dri/card0", F_OK) && access("/dev/fb0", F_OK) && !access("/tmp/emu/fb", F_OK))
        setenv("EMU_FB", "/tmp/emu/fb", 1);
    if (argc > 1 && !strcmp(argv[1], "--version")) { puts(VERSION); return 0; }
    return run_app();
}
