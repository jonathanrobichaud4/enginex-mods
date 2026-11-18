/*
 * fxbands: any combination of LOW / MID / HIGH for the SC LIVE 4's FX unit.
 *
 * Engine's FX frequency control is one filter, so it can put the effect on the lows,
 * the highs, or one side of a cutoff, but never on LOW + HIGH with MID left dry.
 * This library is preloaded into Engine after EngineX's hook. EngineX already wraps
 * every FX unit's processing (hook_fx_common_chain: dry block in, effected block out);
 * fxbands sits in front of that wrapper, splits the dry and the effected signal into
 * three bands with Linkwitz-Riley crossovers (256 Hz and 2.6 kHz, the points Engine's
 * own frequency readout uses) and puts them back together with the effect only in the
 * bands that are switched on:
 *
 *     out = sum over bands b of  (on_b ? band_b(wet) : band_b(dry))
 *
 * Each band fades over 15 ms when it is switched. With all three bands on (or no
 * selection file) the effected block passes through untouched, so stock behaviour and
 * the slider mode are exactly as before.
 *
 * The selection comes from /tmp/enginex-fxbands, an INI file with "mask=N" (bit 0 LOW,
 * bit 1 MID, bit 2 HIGH) written by the FX bar (fx-channel-indicator mod). A thread
 * checks it every 10 ms. Status goes to /tmp/enginex-fxbands.status, also INI
 * ([fxbands] installed=1 ...), which is how the FX bar knows band mode is available.
 *
 * Installation: EngineX patches the entry of Engine's FX chain function with
 * "ldr pc, [pc, #-4]; .word hook". That word is read as data, so fxbands replaces it
 * with its own function (keeping EngineX's as the one it calls) with a single aligned
 * store; there is no moment where the entry is half written. If the entry isn't
 * EngineX's jump (another Engine build, or EngineX's DSP hooks not installed), fxbands
 * does nothing.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#define MASK_FILE "/tmp/enginex-fxbands"
#define STATUS_FILE "/tmp/enginex-fxbands.status"
#define ALL 7

typedef void (*chain_fn)(void *, float *const *, float *const *, uint32_t);

/* Engine 5.3.0 for SC LIVE 4 (GNU build ID) and its FX chain function. */
static const unsigned char build_sclive4[20] = {0xb9, 0x46, 0xc3, 0xb8, 0xd9, 0x9d, 0x32, 0x99, 0x8c, 0x81,
                                                0x97, 0xd5, 0xdc, 0x90, 0x5a, 0x6b, 0xdd, 0xaa, 0x5d, 0x2f};
static const uint32_t chain_offset = 0x172e668;

static chain_fn next_chain;      /* EngineX's hook_fx_common_chain */
static volatile int g_mask = ALL; /* written by the poll thread */
static volatile int g_wet_only_test; /* "test=1" in the file: wet replaced by silence (for checking the split) */
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
    void *common;          /* which FX chain this slot belongs to (0 = free) */
    int busy;              /* taken by an audio thread right now */
    split_t dry[2], wet[2];
    float gain[3];         /* current band gains, 0 = dry, 1 = effected */
    float engaged;         /* 0 = pass the effected block through untouched, 1 = band mix */
    float dl[MAXF], dr[MAXF];
} slot_t;
static slot_t slots[SLOTS];

static slot_t *slot_enter(void *common) {
    for (int i = 0; i < SLOTS; i++)
        if (__atomic_load_n(&slots[i].common, __ATOMIC_ACQUIRE) == common) {
            int z = 0;
            return __atomic_compare_exchange_n(&slots[i].busy, &z, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ? &slots[i] : NULL;
        }
    for (int i = 0; i < SLOTS; i++) {
        void *none = NULL;
        if (__atomic_load_n(&slots[i].common, __ATOMIC_RELAXED) == NULL &&
            __atomic_compare_exchange_n(&slots[i].common, &none, common, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
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
static void slot_exit(slot_t *s) { if (s) __atomic_store_n(&s->busy, 0, __ATOMIC_RELEASE); }

static void fxbands_chain(void *common, float *const *in, float *const *out, uint32_t frames) {
    __atomic_fetch_add(&g_blocks, 1, __ATOMIC_RELAXED);
    int mask = __atomic_load_n(&g_mask, __ATOMIC_RELAXED);
    slot_t *s = (in && out && in[0] && in[1] && out[0] && out[1] && frames && frames <= MAXF) ? slot_enter(common) : NULL;
    if (!s) { next_chain(common, in, out, frames); return; }
    /* Nothing to do and nothing fading: plain pass-through (the filters rest). */
    if (mask == ALL && s->engaged == 0.0f) { slot_exit(s); next_chain(common, in, out, frames); return; }
    if (s->engaged == 0.0f) { memset(s->dry, 0, sizeof s->dry); memset(s->wet, 0, sizeof s->wet); } /* fresh start, no stale tails */
    memcpy(s->dl, in[0], frames * sizeof(float)); /* in and out may be the same buffers */
    memcpy(s->dr, in[1], frames * sizeof(float));
    next_chain(common, in, out, frames);
    __atomic_fetch_add(&g_band_blocks, 1, __ATOMIC_RELAXED);
    const float step = 1.0f / (0.015f * g_rate); /* 15 ms fades */
    float target[3] = {(float)(mask & 1), (float)((mask >> 1) & 1), (float)((mask >> 2) & 1)};
    float eng_target = mask == ALL ? 0.0f : 1.0f;
    int test = g_wet_only_test;
    for (uint32_t i = 0; i < frames; i++) {
        for (int b = 0; b < 3; b++) {
            if (s->gain[b] < target[b]) { s->gain[b] += step; if (s->gain[b] > target[b]) s->gain[b] = target[b]; }
            else if (s->gain[b] > target[b]) { s->gain[b] -= step; if (s->gain[b] < target[b]) s->gain[b] = target[b]; }
        }
        if (s->engaged < eng_target) { s->engaged += step; if (s->engaged > 1) s->engaged = 1; }
        else if (s->engaged > eng_target) { s->engaged -= step; if (s->engaged < 0) s->engaged = 0; }
        for (int ch = 0; ch < 2; ch++) {
            float x = ch ? s->dr[i] : s->dl[i];
            float w = test ? 0.0f : out[ch][i];
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
            if (m) { int v = atoi(m + 5); if (v >= 1 && v <= 7) mask = v; }
            char *t = strstr(buf, "test=");
            if (t) test = atoi(t + 5) == 1;
            char *r = strstr(buf, "rate=");
            if (r) { float v = (float)atof(r + 5); if (v >= 8000 && v <= 192000 && v != g_rate) { g_rate = v; } }
        }
    }
    g_wet_only_test = test;
    __atomic_store_n(&g_mask, mask, __ATOMIC_RELAXED);
}
static void write_status(void) {
    char t[200];
    int n = snprintf(t, sizeof t, "[fxbands]\ninstalled=%d\nmask=%d\nblocks=%u\nband_blocks=%u\nrate=%.0f\n", g_installed,
                     g_mask, __atomic_load_n(&g_blocks, __ATOMIC_RELAXED), __atomic_load_n(&g_band_blocks, __ATOMIC_RELAXED), g_rate);
    int fd = open(STATUS_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return;
    if (write(fd, t, (size_t)n) < 0) {}
    close(fd);
    rename(STATUS_FILE ".tmp", STATUS_FILE);
}

/* ------------------------------------------------------------------ installing */
static uintptr_t engine_base;
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

/* 1 installed, 0 not yet (EngineX hasn't patched the entry), -1 never (wrong build/layout) */
static int try_install(void) {
#if defined(__arm__)
    if (!engine_base) return -1;
    uint32_t *entry = (uint32_t *)(engine_base + chain_offset);
    if (entry[0] != 0xe51ff004) {
        /* stock prologue: EngineX not there yet (or not hooking DSP) */
        static const uint32_t stock[2] = {0xe16d42f4, 0xe1cd60f8};
        return memcmp(entry, stock, 8) ? -1 : 0;
    }
    uint32_t cur = __atomic_load_n(&entry[1], __ATOMIC_ACQUIRE);
    if (cur == (uint32_t)(uintptr_t)fxbands_chain) return 1;
    Dl_info di;
    if (!dladdr((void *)(uintptr_t)cur, &di) || !di.dli_fname || !strstr(di.dli_fname, "libengine-ui-hook")) return -1;
    next_chain = (chain_fn)(uintptr_t)cur;
    long pg = sysconf(_SC_PAGESIZE);
    uintptr_t start = (uintptr_t)&entry[1] & ~(uintptr_t)(pg - 1);
    if (mprotect((void *)start, (size_t)pg, PROT_READ | PROT_WRITE | PROT_EXEC)) return -1;
    __atomic_store_n(&entry[1], (uint32_t)(uintptr_t)fxbands_chain, __ATOMIC_RELEASE);
    mprotect((void *)start, (size_t)pg, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)entry, (char *)(entry + 2));
    return 1;
#else
    return -1;
#endif
}

static void *poll_thread(void *arg) {
    (void)arg;
    int state = 0;
    for (int tries = 0; state == 0 && tries < 600; tries++) { /* up to 60 s for EngineX to install */
        state = try_install();
        if (state == 0) usleep(100000);
    }
    g_installed = state == 1;
    fprintf(stderr, "fxbands: %s\n", g_installed ? "installed on the FX chain" : "not installed (EngineX DSP hook not found)");
    write_status();
    if (!g_installed) return NULL;
    float rate = g_rate;
    for (unsigned tick = 0;; tick++) {
        read_mask();
        if (g_rate != rate) {
            rate = g_rate; /* rebuild the crossovers (a click at most; only happens on a rate change) */
            lp1 = butter(256, rate, 0); hp1 = butter(256, rate, 1);
            lp2 = butter(2600, rate, 0); hp2 = butter(2600, rate, 1);
        }
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
    return strstr(exe, "/Engine/Engine") != NULL || strstr(exe, "qemu-arm") != NULL;
}

__attribute__((constructor)) static void fxbands_init(void) {
    if (!is_engine()) return;
    dl_iterate_phdr(find_engine, NULL);
    if (!engine_base) return;
    lp1 = butter(256, g_rate, 0); hp1 = butter(256, g_rate, 1);
    lp2 = butter(2600, g_rate, 0); hp2 = butter(2600, g_rate, 1);
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 64 * 1024);
    pthread_create(&t, &a, poll_thread, NULL);
    pthread_attr_destroy(&a);
}
