/* sc-diagnostics: drawing and screen output (see gfx.h). */
#define _GNU_SOURCE
#include "gfx.h"
#include "assets.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

uint32_t canvas[CW * CH];

static enum { OUT_NONE, OUT_KMS, OUT_FB, OUT_EMU } out;
static int rot;               /* 0, 90, 180, 270 */
static int pw, ph, pstride;   /* panel size and bytes per row */
static uint8_t *pmem;
static size_t psize;
static int dfd = -1, ffd = -1;
static uint32_t fb_id, crtc_id, conn_id, dumb_handle;
static drmModeCrtc *saved_crtc;

uint32_t rgb(int r, int g, int b) { return 0xFF000000u | (uint32_t)(r & 255) << 16 | (uint32_t)(g & 255) << 8 | (uint32_t)(b & 255); }
uint32_t mixc(uint32_t a, uint32_t b, double t) {
    if (t <= 0) return a;
    if (t >= 1) return b;
    int r = (int)(((a >> 16) & 255) * (1 - t) + ((b >> 16) & 255) * t);
    int g = (int)(((a >> 8) & 255) * (1 - t) + ((b >> 8) & 255) * t);
    int bl = (int)((a & 255) * (1 - t) + (b & 255) * t);
    return rgb(r, g, bl);
}
int gfx_rotation(void) { return rot; }
void gfx_panel_size(int *w, int *h) { *w = pw; *h = ph; }
const char *gfx_output_name(void) {
    return out == OUT_KMS ? "KMS" : out == OUT_FB ? "framebuffer" : out == OUT_EMU ? "emulator" : "none";
}

/* The panel's rotation from the devicetree, read the way Engine's setup-screenrotation.sh does
 * (two bytes at offset 2). DIAG_ROTATE overrides it. */
static int dt_rotation(void) {
    const char *e = getenv("DIAG_ROTATE");
    if (e && *e) return ((atoi(e) % 360) + 360) % 360;
    static const char *files[] = {
        "/sys/firmware/devicetree/base/dsi@fde20000/panel@0/rotation",
        "/sys/firmware/devicetree/base/dsi@fe060000/panel@0/rotation",
        "/sys/firmware/devicetree/base/mipi@ff960000/panel@0/rotation",
    };
    for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++) {
        FILE *f = fopen(files[i], "rb");
        if (!f) continue;
        unsigned char b[4] = {0};
        size_t n = fread(b, 1, 4, f);
        fclose(f);
        if (n >= 4) {
            int deg = b[2] << 8 | b[3];
            if (deg % 90 == 0) return deg % 360;
        }
    }
    return 0;
}

static int open_kms(void) {
    dfd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (dfd < 0) return -1;
    drmModeRes *res = drmModeGetResources(dfd);
    if (!res) goto fail;
    drmModeConnector *conn = NULL;
    for (int i = 0; i < res->count_connectors && !conn; i++) {
        drmModeConnector *c = drmModeGetConnector(dfd, res->connectors[i]);
        if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) conn = c;
        else if (c) drmModeFreeConnector(c);
    }
    if (!conn) { drmModeFreeResources(res); goto fail; }
    drmModeModeInfo mode = conn->modes[0];
    for (int i = 0; i < conn->count_modes; i++)
        if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) { mode = conn->modes[i]; break; }
    conn_id = conn->connector_id;
    crtc_id = 0;
    if (conn->encoder_id) {
        drmModeEncoder *enc = drmModeGetEncoder(dfd, conn->encoder_id);
        if (enc) { crtc_id = enc->crtc_id; drmModeFreeEncoder(enc); }
    }
    for (int i = 0; !crtc_id && i < conn->count_encoders; i++) {
        drmModeEncoder *enc = drmModeGetEncoder(dfd, conn->encoders[i]);
        if (!enc) continue;
        for (int j = 0; j < res->count_crtcs; j++)
            if (enc->possible_crtcs & (1u << j)) { crtc_id = res->crtcs[j]; break; }
        drmModeFreeEncoder(enc);
    }
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    if (!crtc_id) goto fail;
    pw = mode.hdisplay; ph = mode.vdisplay;
    struct drm_mode_create_dumb cd = {.width = (uint32_t)pw, .height = (uint32_t)ph, .bpp = 32};
    if (drmIoctl(dfd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) goto fail;
    dumb_handle = cd.handle;
    pstride = (int)cd.pitch;
    psize = cd.size;
    if (drmModeAddFB(dfd, (uint32_t)pw, (uint32_t)ph, 24, 32, cd.pitch, cd.handle, &fb_id)) goto fail;
    struct drm_mode_map_dumb md = {.handle = cd.handle};
    if (drmIoctl(dfd, DRM_IOCTL_MODE_MAP_DUMB, &md)) goto fail;
    pmem = mmap(NULL, psize, PROT_READ | PROT_WRITE, MAP_SHARED, dfd, (off_t)md.offset);
    if (pmem == MAP_FAILED) { pmem = NULL; goto fail; }
    memset(pmem, 0, psize);
    saved_crtc = drmModeGetCrtc(dfd, crtc_id);
    if (drmModeSetCrtc(dfd, crtc_id, fb_id, 0, 0, &conn_id, 1, &mode)) {
        fprintf(stderr, "diag: KMS modeset failed: %s\n", strerror(errno));
        goto fail;
    }
    out = OUT_KMS;
    return 0;
fail:
    if (dfd >= 0) close(dfd);
    dfd = -1;
    return -1;
}

static int open_fbdev(void) {
    ffd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (ffd < 0) return -1;
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (ioctl(ffd, FBIOGET_VSCREENINFO, &v) || ioctl(ffd, FBIOGET_FSCREENINFO, &f) || v.bits_per_pixel != 32) {
        close(ffd); ffd = -1; return -1;
    }
    pw = (int)v.xres; ph = (int)v.yres; pstride = (int)f.line_length;
    psize = (size_t)f.smem_len;
    pmem = mmap(NULL, psize, PROT_READ | PROT_WRITE, MAP_SHARED, ffd, 0);
    if (pmem == MAP_FAILED) { pmem = NULL; close(ffd); ffd = -1; return -1; }
    out = OUT_FB;
    return 0;
}

/* The SC LIVE 4 emulator's screen file: 32-byte header ("EMUF", w, h, bytes per row, format,
 * frame counter) then the pixels. */
static int open_emu(const char *path) {
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return -1;
    psize = 32 + (size_t)CW * CH * 4;
    struct stat st;
    if (fstat(fd, &st) == 0 && (size_t)st.st_size < psize && ftruncate(fd, (off_t)psize)) { close(fd); return -1; }
    pmem = mmap(NULL, psize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (pmem == MAP_FAILED) { pmem = NULL; return -1; }
    pw = CW; ph = CH; pstride = CW * 4;
    out = OUT_EMU;
    return 0;
}

int gfx_open(void) {
    const char *emu = getenv("EMU_FB");
    if (emu && *emu) {
        rot = 0;
        if (open_emu(emu) == 0) return 0;
    }
    if (open_kms() && open_fbdev()) { fprintf(stderr, "diag: no screen (no /dev/dri/card0 or /dev/fb0)\n"); return -1; }
    rot = dt_rotation();
    /* If the panel is already landscape, Engine's rotation is about which way up it is. */
    if ((rot == 90 || rot == 270) && pw >= ph) rot = (rot == 90) ? 0 : 180;
    if ((rot == 0 || rot == 180) && ph > pw) rot = 90;
    fprintf(stderr, "diag: screen %s %dx%d, canvas turned %d degrees\n", gfx_output_name(), pw, ph, rot);
    return 0;
}

void gfx_present(void) {
    if (!pmem) return;
    if (out == OUT_EMU) {
        uint32_t *h = (uint32_t *)pmem;
        memcpy(pmem + 32, canvas, sizeof canvas);
        h[1] = CW; h[2] = CH; h[3] = CW * 4; h[4] = 5;
        __atomic_store_n(&h[5], h[5] + 1, __ATOMIC_RELEASE);
        memcpy(pmem, "EMUF", 4);
        return;
    }
    /* Fit the canvas onto the panel, turned by rot, scaled by the nearest whole pixel map. */
    int lw = (rot == 90 || rot == 270) ? CH : CW, lh = (rot == 90 || rot == 270) ? CW : CH;
    for (int py = 0; py < ph; py++) {
        uint32_t *row = (uint32_t *)(pmem + (size_t)py * pstride);
        int ly = py * lh / ph;
        for (int px = 0; px < pw; px++) {
            int lx = px * lw / pw, cx, cy;
            switch (rot) {
            case 90: cx = ly; cy = CH - 1 - lx; break;
            case 180: cx = CW - 1 - lx; cy = CH - 1 - ly; break;
            case 270: cx = CW - 1 - ly; cy = lx; break;
            default: cx = lx; cy = ly;
            }
            row[px] = canvas[cy * CW + cx];
        }
    }
}

void gfx_close(void) {
    if (out == OUT_KMS && dfd >= 0) {
        if (saved_crtc) {
            drmModeSetCrtc(dfd, saved_crtc->crtc_id, saved_crtc->buffer_id, saved_crtc->x, saved_crtc->y,
                           &conn_id, 1, &saved_crtc->mode);
            drmModeFreeCrtc(saved_crtc);
        }
        if (pmem) munmap(pmem, psize);
        drmModeRmFB(dfd, fb_id);
        struct drm_mode_destroy_dumb dd = {.handle = dumb_handle};
        drmIoctl(dfd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
        close(dfd);
    } else if (out == OUT_FB) {
        memset(pmem, 0, psize);
        munmap(pmem, psize);
        close(ffd);
    } else if (out == OUT_EMU && pmem) {
        munmap(pmem, psize);
    }
    out = OUT_NONE;
    pmem = NULL;
}

/* ---- drawing ---- */
static inline void px(int x, int y, uint32_t c) {
    if ((unsigned)x < CW && (unsigned)y < CH) canvas[y * CW + x] = c;
}
void fill(int x, int y, int w, int h, uint32_t c) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > CW ? CW : x + w, y1 = y + h > CH ? CH : y + h;
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++) canvas[j * CW + i] = c;
}
void frame(int x, int y, int w, int h, int t, uint32_t c) {
    fill(x, y, w, t, c); fill(x, y + h - t, w, t, c); fill(x, y, t, h, c); fill(x + w - t, y, t, h, c);
}
void fill_round(int x, int y, int w, int h, int r, uint32_t c) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int j = 0; j < h; j++) {
        int inset = 0;
        int dy = j < r ? r - j : (j >= h - r ? j - (h - r - 1) : 0);
        if (dy) inset = r - (int)sqrt((double)(r * r - dy * dy));
        fill(x + inset, y + j, w - 2 * inset, 1, c);
    }
}
void fill_circle(int cx, int cy, int r, uint32_t c) {
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)sqrt((double)(r * r - dy * dy));
        fill(cx - dx, cy + dy, 2 * dx + 1, 1, c);
    }
}
void ring(int cx, int cy, int r, int t, uint32_t c) { arc(cx, cy, r, t, 0, 2 * M_PI, c); }
void arc(int cx, int cy, int r, int t, double a0, double a1, uint32_t c) {
    int ro = r, ri = r - t;
    for (int dy = -ro; dy <= ro; dy++)
        for (int dx = -ro; dx <= ro; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 > ro * ro || d2 < ri * ri) continue;
            double a = atan2((double)dx, (double)-dy); /* 0 at 12 o'clock, clockwise */
            if (a < 0) a += 2 * M_PI;
            double s = fmod(a0, 2 * M_PI), e = a1;
            if (s < 0) s += 2 * M_PI;
            e = s + (a1 - a0);
            if ((a >= s && a <= e) || (a + 2 * M_PI >= s && a + 2 * M_PI <= e)) px(cx + dx, cy + dy, c);
        }
}
void line(int x0, int y0, int x1, int y1, int t, uint32_t c) {
    int n = abs(x1 - x0) > abs(y1 - y0) ? abs(x1 - x0) : abs(y1 - y0);
    if (n == 0) n = 1;
    for (int i = 0; i <= n; i++) {
        int x = x0 + (x1 - x0) * i / n, y = y0 + (y1 - y0) * i / n;
        fill(x - t / 2, y - t / 2, t, t, c);
    }
}

static void font_of(int size, const glyph_t **g, const uint8_t **d, int *asc, int *h) {
    if (size == FL) { *g = FONTL_G; *d = FONTL_D; *asc = FONTL_ASC; *h = FONTL_H; }
    else if (size == FM) { *g = FONTM_G; *d = FONTM_D; *asc = FONTM_ASC; *h = FONTM_H; }
    else { *g = FONTS_G; *d = FONTS_D; *asc = FONTS_ASC; *h = FONTS_H; }
}
int text_h(int size) { const glyph_t *g; const uint8_t *d; int a, h; font_of(size, &g, &d, &a, &h); return h; }
int text_w(int size, const char *s) {
    const glyph_t *g; const uint8_t *d; int a, h, w = 0;
    font_of(size, &g, &d, &a, &h);
    for (; *s; s++) { int k = (unsigned char)*s; if (k < 32 || k > 126) k = '?'; w += g[k - 32].adv; }
    return w;
}
int text(int x, int y, int size, uint32_t c, const char *s) {
    const glyph_t *g; const uint8_t *d; int a, h, x0 = x;
    font_of(size, &g, &d, &a, &h);
    for (; *s; s++) {
        int k = (unsigned char)*s;
        if (k < 32 || k > 126) k = '?';
        const glyph_t *q = &g[k - 32];
        const uint8_t *bm = d + q->off;
        for (int j = 0; j < q->h; j++) {
            int yy = y + q->top + j;
            if ((unsigned)yy >= CH) continue;
            for (int i = 0; i < q->w; i++) {
                int v = bm[j * q->w + i], xx = x + i;
                if (!v || (unsigned)xx >= CW) continue;
                uint32_t *p = &canvas[yy * CW + xx];
                *p = v == 255 ? c : mixc(*p, c, v / 255.0);
            }
        }
        x += q->adv;
    }
    return x - x0;
}
void text_c(int cx, int y, int size, uint32_t c, const char *s) { text(cx - text_w(size, s) / 2, y, size, c, s); }
