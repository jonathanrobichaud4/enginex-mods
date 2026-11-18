/* sc-diagnostics: drawing and screen output.
 *
 * Everything is drawn into a 1280x800 XRGB canvas (the SC LIVE 4's screen as Engine shows it)
 * and copied to the panel by one of three outputs:
 *   KMS          /dev/dri/card0, a dumb buffer on the connected connector, rotated to the
 *                panel the way Engine's runengine scripts do (devicetree panel rotation)
 *   framebuffer  /dev/fb0, if KMS isn't available
 *   emulator     the SC LIVE 4 emulator's frame file (EMU_FB), for testing on a PC
 */
#pragma once
#include <stdint.h>

enum { CW = 1280, CH = 800 };
extern uint32_t canvas[CW * CH];

int gfx_open(void);              /* 0 ok; picks an output, logs which */
void gfx_present(void);          /* copy the canvas to the screen */
void gfx_close(void);
int gfx_rotation(void);          /* degrees the canvas is turned onto the panel: 0/90/180/270 */
void gfx_panel_size(int *w, int *h); /* the panel's native mode */
const char *gfx_output_name(void);

void fill(int x, int y, int w, int h, uint32_t c);
void frame(int x, int y, int w, int h, int t, uint32_t c);
void fill_round(int x, int y, int w, int h, int r, uint32_t c);
void fill_circle(int cx, int cy, int r, uint32_t c);
void ring(int cx, int cy, int r, int t, uint32_t c);
void arc(int cx, int cy, int r, int t, double a0, double a1, uint32_t c); /* radians, 0 = 12 o'clock, clockwise */
void line(int x0, int y0, int x1, int y1, int t, uint32_t c);
enum { FS, FM, FL };
int text(int x, int y, int size, uint32_t c, const char *s);  /* y = top; returns width */
int text_w(int size, const char *s);
int text_h(int size);
void text_c(int cx, int y, int size, uint32_t c, const char *s); /* centred on cx */
uint32_t rgb(int r, int g, int b);
uint32_t mixc(uint32_t a, uint32_t b, double t);
