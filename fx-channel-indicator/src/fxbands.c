/*
 * fxbands: any combination of LOW / MID / HIGH for the SC LIVE 4's FX unit.
 *
 * Engine's FX frequency control is one filter, so it can put the effect on the lows,
 * the highs, or one side of a cutoff, but never on LOW + HIGH with MID left dry.
 * This library is preloaded into Engine (after EngineX's hook) and hooks the FX unit's
 * own process function: (unit, dry in, out, frames). That function runs the effect
 * into the unit's wet buffers and mixes it with the dry input into out (dry + (wet -
 * dry) * amount, or dry + wet * amount; Engine has three versions of it). fxbands keeps
 * a copy of the dry input, lets the function run, then splits both the dry and the
 * finished sound into three bands with Linkwitz-Riley crossovers (256 Hz and 2.6 kHz,
 * the points Engine's own frequency readout uses) and rebuilds the output with the
 * finished sound only in the selected bands:
 *
 *     out = sum over bands b of  (on_b ? band_b(finished) : band_b(dry))
 *
 * Units whose output equals their input (Engine keeps several, most idle) are left alone.
 * Each band fades over 15 ms when it is switched. With all three bands on (or no
 * selection file) the unit's output passes through untouched; with none on, only the
 * dry sound is left (the effect keeps running, so its tail is there again when a band
 * comes back).
 *
 * The selection comes from /tmp/enginex-fxbands, an INI file with "mask=N" (bit 0 LOW,
 * bit 1 MID, bit 2 HIGH, 0 = none) written by the FX bar (fx-channel-indicator mod).
 * A thread checks it every 10 ms. Status goes to /tmp/enginex-fxbands.status, also INI
 * ([fxbands] installed=1 ...), which is how the FX bar knows band mode is available.
 *
 * Installation: in the constructor, before Engine starts any threads, each of the three
 * functions gets "ldr pc, [pc, #-4]; .word hook" at its entry, but only if the entry
 * still holds the two expected instructions (moved to a small trampoline). Only Engine
 * 5.3.0 for SC LIVE 4 (checked by GNU build ID) is touched.
 *
 * Debug ("test=N" in the selection file): 1 = finished sound replaced by silence (checks
 * the split), 2 = output the unit's dry input, 3 = per-unit levels in the status file.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <unistd.h>

#define MASK_FILE "/tmp/enginex-fxbands"
#define STATUS_FILE "/tmp/enginex-fxbands.status"
#define ALL 7

typedef void (*unit_fn)(void *, float *const *, float *const *, uint32_t);

/* Engine 5.3.0 for SC LIVE 4 (GNU build ID) and its FX unit process functions. */
static const unsigned char build_sclive4[20] = {0xb9, 0x46, 0xc3, 0xb8, 0xd9, 0x9d, 0x32, 0x99, 0x8c, 0x81,
                                                0x97, 0xd5, 0xdc, 0x90, 0x5a, 0x6b, 0xdd, 0xaa, 0x5d, 0x2f};
#define NUNITS 3
static const uint32_t unit_offsets[NUNITS] = {0xe78008, 0xe78130, 0xe9f73c};
static const uint32_t unit_prologue[2] = {0xe16d42f4, 0xe1cd60f8}; /* strd r4,[sp,#-36]! ; strd r6,[sp,#8] */
static unit_fn unit_orig[NUNITS]; /* trampolines: the two moved instructions, then the rest */

static uintptr_t engine_base;
static volatile int g_mask = ALL;
static volatile int g_test;
static float g_rate = 44100.0f;
static uint32_t g_blocks, g_band_blocks;
static int g_installed;

/* ------------------------------------------------------------------ filters */
typedef struct { float b0, b1, b2, a1, a2; } coef_t;
typedef struct { float z1, z2; } bq_t;

static float bq(const coef_t *c, bq_t *s, float x) {
    float y = c->b0 * x + s->z1;
    s->z1 = c->b1 * x - c->a1 * y + s->z2;
    s->z2 = c->b2 * x - c->a2 * y;
    return y;
}
/* 2nd-order Butterworth low/high pass; two in a row make a Linkwitz-Riley (LR4) section. */
static coef_t butter(float fc, float rate, int high) {
    float w = 2.0f * (float)M_PI * fc / rate, cw = cosf(w), al = sinf(w) / (2.0f * 0.70710678f);
    float a0 = 1 + al;
    coef_t c;
    if (high) { c.b0 = (1 + cw) / 2; c.b1 = -(1 + cw); c.b2 = (1 + cw) / 2; }
    else { c.b0 = (1 - cw) / 2; c.b1 = 1 - cw; c.b2 = (1 - cw) / 2; }
    c.a1 = -2 * cw; c.a2 = 1 - al;
    c.b0 /= a0; c.b1 /= a0; c.b2 /= a0; c.a1 /= a0; c.a2 /= a0;
    return c;
}
static coef_t lp1, hp1, lp2, hp2; /* crossover 1 (256 Hz) and 2 (2.6 kHz) */
static void make_filters(float rate) {
    lp1 = butter(256, rate, 0); hp1 = butter(256, rate, 1);
    lp2 = butter(2600, rate, 0); hp2 = butter(2600, rate, 1);
}

/* One signal's 3-band split: 12 biquads. The low band also goes through the second
 * crossover (low+high of it, an all-pass) so all three bands line up in phase. */
typedef struct { bq_t s[12]; } split_t;
static void split(split_t *p, float x, float out[3]) {
    float lo = bq(&lp1, &p->s[1], bq(&lp1, &p->s[0], x));
    float rest = bq(&hp1, &p->s[3], bq(&hp1, &p->s[2], x));
    out[1] = bq(&lp2, &p->s[5], bq(&lp2, &p->s[4], rest));
    out[2] = bq(&hp2, &p->s[7], bq(&hp2, &p->s[6], rest));
    out[0] = bq(&lp2, &p->s[9], bq(&lp2, &p->s[8], lo)) + bq(&hp2, &p->s[11], bq(&hp2, &p->s[10], lo));
}

/* ------------------------------------------------------------------ per FX unit state */
#define SLOTS 16
#define MAXF 4096
typedef struct {
    void *unit;    /* which FX unit this slot belongs to (0 = free) */
    int busy;      /* taken by an audio thread right now */
    split_t dry[2], wet[2];
    float gain[3]; /* current band gains, 0 = dry, 1 = finished */
    float engaged; /* 0 = pass the unit's output through untouched, 1 = band mix */
    float dl[MAXF], dr[MAXF];
    uint32_t calls, frames_seen, variant; double e_in, e_out; /* test=3 */
} slot_t;
static slot_t slots[SLOTS];

static slot_t *slot_enter(void *unit) {
    for (int i = 0; i < SLOTS; i++)
        if (__atomic_load_n(&slots[i].unit, __ATOMIC_ACQUIRE) == unit) {
            int z = 0;
            return __atomic_compare_exchange_n(&slots[i].busy, &z, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ? &slots[i] : NULL;
        }
    for (int i = 0; i < SLOTS; i++) {
        void *none = NULL;
        if (__atomic_load_n(&slots[i].unit, __ATOMIC_RELAXED) == NULL &&
            __atomic_compare_exchange_n(&slots[i].unit, &none, unit, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            int z = 0;
            if (!__atomic_compare_exchange_n(&slots[i].busy, &z, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return NULL;
            slot_t *s = &slots[i];
            memset(s->dry, 0, sizeof s->dry); memset(s->wet, 0, sizeof s->wet);
            s->gain[0] = s->gain[1] = s->gain[2] = 1; s->engaged = 0;
            return s;
        }
    }
    return NULL;
}
static void slot_exit(slot_t *s) { __atomic_store_n(&s->busy, 0, __ATOMIC_RELEASE); }

static void fxbands_unit(int k, void *unit, float *const *in, float *const *out, uint32_t frames) {
    unit_fn next = unit_orig[k];
    __atomic_fetch_add(&g_blocks, 1, __ATOMIC_RELAXED);
    int mask = __atomic_load_n(&g_mask, __ATOMIC_RELAXED), test = g_test;
    slot_t *s = (in && out && in[0] && in[1] && out[0] && out[1] && frames && frames <= MAXF) ? slot_enter(unit) : NULL;
    if (!s) { next(unit, in, out, frames); return; }
    /* Nothing to do and nothing fading: plain pass-through (the filters rest). */
    if (mask == ALL && s->engaged == 0.0f && !test) { slot_exit(s); next(unit, in, out, frames); return; }
    if (s->engaged == 0.0f) { memset(s->dry, 0, sizeof s->dry); memset(s->wet, 0, sizeof s->wet); } /* no stale tails */
    memcpy(s->dl, in[0], frames * sizeof(float)); /* in and out may be the same buffers */
    memcpy(s->dr, in[1], frames * sizeof(float));
    next(unit, in, out, frames);
    if (test == 3) {
        double ei = 0, eo = 0;
        for (uint32_t i = 0; i < frames; i++) { ei += s->dl[i] * s->dl[i]; eo += out[0][i] * out[0][i]; }
        s->calls++; s->frames_seen = frames; s->variant = (uint32_t)k; s->e_in += ei; s->e_out += eo;
        slot_exit(s); return;
    }
    if (test == 2) {
        memcpy(out[0], s->dl, frames * sizeof(float)); memcpy(out[1], s->dr, frames * sizeof(float));
        slot_exit(s); return;
    }
    /* Engine keeps several FX units; the idle ones hand their input straight back. Leave
       those alone (no filtering, no phase shift), fading out if one just went idle. */
    int active = memcmp(out[0], s->dl, frames * sizeof(float)) || memcmp(out[1], s->dr, frames * sizeof(float));
    if (!active && s->engaged == 0.0f) { slot_exit(s); return; }
    __atomic_fetch_add(&g_band_blocks, 1, __ATOMIC_RELAXED);
    const float step = 1.0f / (0.015f * g_rate); /* 15 ms fades */
    float target[3] = {(float)(mask & 1), (float)((mask >> 1) & 1), (float)((mask >> 2) & 1)};
    float eng_target = (mask == ALL || !active) ? 0.0f : 1.0f;
    for (uint32_t i = 0; i < frames; i++) {
        for (int b = 0; b < 3; b++) {
            if (s->gain[b] < target[b]) { s->gain[b] += step; if (s->gain[b] > target[b]) s->gain[b] = target[b]; }
            else if (s->gain[b] > target[b]) { s->gain[b] -= step; if (s->gain[b] < target[b]) s->gain[b] = target[b]; }
        }
        if (s->engaged < eng_target) { s->engaged += step; if (s->engaged > 1) s->engaged = 1; }
        else if (s->engaged > eng_target) { s->engaged -= step; if (s->engaged < 0) s->engaged = 0; }
        for (int ch = 0; ch < 2; ch++) {
            float x = ch ? s->dr[i] : s->dl[i];
            float w = test == 1 ? 0.0f : out[ch][i];
            float db[3], wb[3];
            split(&s->dry[ch], x, db);
            split(&s->wet[ch], w, wb);
            float mix = 0;
            for (int b = 0; b < 3; b++) mix += db[b] + s->gain[b] * (wb[b] - db[b]);
            float y = w + s->engaged * (mix - w);
            out[ch][i] = isfinite(y) ? y : 0.0f;
        }
    }
    slot_exit(s);
}
static void unit0(void *u, float *const *i, float *const *o, uint32_t f) { fxbands_unit(0, u, i, o, f); }
static void unit1(void *u, float *const *i, float *const *o, uint32_t f) { fxbands_unit(1, u, i, o, f); }
static void unit2(void *u, float *const *i, float *const *o, uint32_t f) { fxbands_unit(2, u, i, o, f); }
static unit_fn const unit_hooks[NUNITS] = {unit0, unit1, unit2};

/* ------------------------------------------------------------------ selection and status */
static void read_mask(void) {
    char buf[96] = {0};
    int fd = open(MASK_FILE, O_RDONLY | O_CLOEXEC);
    int mask = ALL, test = 0;
    if (fd >= 0) {
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n > 0) {
            char *m = strstr(buf, "mask=");
            if (m) { int v = atoi(m + 5); if (v >= 0 && v <= 7) mask = v; }
            char *t = strstr(buf, "test=");
            if (t) test = atoi(t + 5);
            char *r = strstr(buf, "rate=");
            if (r) { float v = (float)atof(r + 5); if (v >= 8000 && v <= 192000) g_rate = v; }
        }
    }
    g_test = test;
    __atomic_store_n(&g_mask, mask, __ATOMIC_RELAXED);
}
static void write_status(void) {
    char t[2048];
    int n = snprintf(t, sizeof t, "[fxbands]\ninstalled=%d\nmask=%d\nblocks=%u\nband_blocks=%u\nrate=%.0f\n", g_installed, g_mask,
                     __atomic_load_n(&g_blocks, __ATOMIC_RELAXED), __atomic_load_n(&g_band_blocks, __ATOMIC_RELAXED), g_rate);
    for (int i = 0; i < SLOTS && n < (int)sizeof t - 200; i++) {
        slot_t *q = &slots[i];
        if (!q->unit || !q->calls) continue;
        double count = (double)q->calls * q->frames_seen + 1;
        n += snprintf(t + n, sizeof t - (size_t)n, "unit%d=%p variant=%u calls=%u frames=%u in=%.5f out=%.5f\n", i, q->unit, q->variant,
                      q->calls, q->frames_seen, sqrt(q->e_in / count), sqrt(q->e_out / count));
        q->calls = 0; q->e_in = q->e_out = 0;
    }
    int fd = open(STATUS_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return;
    if (write(fd, t, (size_t)n) < 0) {}
    close(fd);
    rename(STATUS_FILE ".tmp", STATUS_FILE);
}

/* ------------------------------------------------------------------ installing */
static int find_engine(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size; (void)data;
    for (size_t i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *p = &info->dlpi_phdr[i];
        if (p->p_type != PT_NOTE) continue;
        const unsigned char *at = (const void *)(info->dlpi_addr + p->p_vaddr), *end = at + p->p_memsz;
        while ((size_t)(end - at) >= sizeof(ElfW(Nhdr))) {
            const ElfW(Nhdr) *n = (const void *)at;
            size_t names = ((size_t)n->n_namesz + 3) & ~(size_t)3, desc = ((size_t)n->n_descsz + 3) & ~(size_t)3;
            at += sizeof *n;
            if (names > (size_t)(end - at) || desc > (size_t)(end - at) - names) break;
            if (n->n_type == NT_GNU_BUILD_ID && n->n_namesz == 4 && n->n_descsz == 20 && !memcmp(at + names, build_sclive4, 20))
                engine_base = info->dlpi_addr;
            at += names + desc;
        }
    }
    return 1; /* only the program itself */
}

/* Hooks all three FX unit functions, or none. Runs in the constructor, before Engine has
   threads, so nothing executes the code while it changes. */
static int install(void) {
#if defined(__arm__)
    if (!engine_base) return 0;
    for (int k = 0; k < NUNITS; k++)
        if (memcmp((void *)(engine_base + unit_offsets[k]), unit_prologue, 8)) return 0;
    uint32_t *tr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tr == MAP_FAILED) return 0;
    for (int k = 0; k < NUNITS; k++) {
        uint32_t *t = tr + k * 4;
        t[0] = unit_prologue[0];
        t[1] = unit_prologue[1];
        t[2] = 0xe51ff004; /* ldr pc, [pc, #-4] */
        t[3] = (uint32_t)(engine_base + unit_offsets[k] + 8);
        unit_orig[k] = (unit_fn)(uintptr_t)t;
    }
    if (mprotect(tr, 4096, PROT_READ | PROT_EXEC)) return 0;
    __builtin___clear_cache((char *)tr, (char *)(tr + NUNITS * 4));
    long pg = sysconf(_SC_PAGESIZE);
    for (int k = 0; k < NUNITS; k++) {
        uint32_t *e = (uint32_t *)(engine_base + unit_offsets[k]);
        uintptr_t start = (uintptr_t)e & ~(uintptr_t)(pg - 1);
        uintptr_t end = ((uintptr_t)(e + 2) + (uintptr_t)pg - 1) & ~(uintptr_t)(pg - 1);
        if (mprotect((void *)start, end - start, PROT_READ | PROT_WRITE | PROT_EXEC)) return k ? -1 : 0;
        e[0] = 0xe51ff004;
        e[1] = (uint32_t)(uintptr_t)unit_hooks[k];
        mprotect((void *)start, end - start, PROT_READ | PROT_EXEC);
        __builtin___clear_cache((char *)e, (char *)(e + 2));
    }
    return 1;
#else
    return 0;
#endif
}

static void *poll_thread(void *arg) {
    (void)arg;
    /* Engine's helper processes (offline analyzers) run the same binary and load this too;
       only the first one, the real Engine, gets the lock and owns the status file. */
    int lk = open(STATUS_FILE ".lock", O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lk < 0 || flock(lk, LOCK_EX | LOCK_NB)) { if (lk >= 0) close(lk); return NULL; }
    fprintf(stderr, "fxbands: %s\n", g_installed ? "installed on the FX unit" : "not installed (unexpected Engine code)");
    write_status();
    if (!g_installed) return NULL;
    float rate = g_rate;
    for (unsigned tick = 0;; tick++) {
        read_mask();
        if (g_rate != rate) { rate = g_rate; make_filters(rate); }
        if (tick % 100 == 0) write_status();
        usleep(10000);
    }
    return NULL;
}

static int is_engine(void) {
    char exe[256];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return 0;
    exe[n] = 0;
    return strstr(exe, "/Engine/Engine") != NULL;
}

__attribute__((constructor)) static void fxbands_init(void) {
    if (!is_engine()) return;
    dl_iterate_phdr(find_engine, NULL);
    if (!engine_base) return;
    make_filters(g_rate);
    g_installed = install() == 1;
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 64 * 1024);
    pthread_create(&t, &a, poll_thread, NULL);
    pthread_attr_destroy(&a);
}
