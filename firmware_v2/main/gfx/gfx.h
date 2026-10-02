#pragma once
#include <stdint.h>
#include <stdbool.h>

// Minimal RGB565 software renderer for the 480x480 round panel.
//
// Deliberately not LVGL: this UI is a map, a dot and three buttons, and every
// bug worth fixing in the old tree lived in the widget/event layer (events
// that do not bubble, a theme-less white screen background, a partial redraw
// of a 768 px container at 200 Hz). Here the framebuffer is a plain array
// that we own end to end.
//
// The framebuffer lives in PSRAM and is the single source of truth for what
// is on screen; panel_present() pushes dirty regions to the panel.

#define GFX_W 480
#define GFX_H 480

// RGB565 little-endian, matching the SD tile format and the panel.
typedef uint16_t gfx_color_t;

#define GFX_RGB(r, g, b) \
    ((gfx_color_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

extern gfx_color_t *gfx_fb;  // GFX_W * GFX_H, owned by gfx

// Allocate the framebuffer. Call once, after the heap/PSRAM is up.
void gfx_init(void);

// Clear the whole screen to a colour.
void gfx_clear(gfx_color_t c);

// --- primitives (all clip to the framebuffer) ---
void gfx_pixel(int x, int y, gfx_color_t c);
void gfx_fill_rect(int x, int y, int w, int h, gfx_color_t c);
void gfx_rect_outline(int x, int y, int w, int h, gfx_color_t c);
// Rounded rectangle. r is clamped to w/2 and h/2, so r == w/2 is a full
// capsule and comes out solid. Note the naive "centre rect + two side rects"
// decomposition degenerates to zero width at exactly r*2 == w; do not use it.
void gfx_round_rect(int x, int y, int w, int h, int r, gfx_color_t c);
// Outline of the same shape, t pixels thick.
void gfx_round_rect_outline(int x, int y, int w, int h, int r, int t, gfx_color_t c);
void gfx_circle(int cx, int cy, int r, gfx_color_t c);

// --- text ---
// 5x7 glyphs in a 6x8 cell, uppercase only. scale is an integer zoom.
int gfx_text_width(const char *s, int scale);
void gfx_text(int x, int y, const char *s, gfx_color_t c, int scale);
void gfx_text_centred(int cx, int y, const char *s, gfx_color_t c, int scale);

// --- blitting ---
// Copy a w*h RGB565 block to (x,y), clipped to the framebuffer. src points at
// the block's own top-left pixel.
void gfx_blit(int x, int y, int w, int h, const gfx_color_t *src);
// As above with an explicit source stride/height, for sub-rectangles of a
// wider image.
void gfx_blit_strided(int x, int y, int w, int h, const gfx_color_t *src,
                      int src_stride, int src_h);
// As above, but src is the whole image and (src_x, src_y) is where the block
// starts inside it. Prefer this over pre-offsetting src by hand: the clipping
// adjusts the source row and column itself, so a pre-offset pointer plus a
// clipped row skips the source twice. src_h is the source's height and the
// call is refused outright if the block would read past it.
void gfx_blit_region(int x, int y, int w, int h, const gfx_color_t *src,
                     int src_stride, int src_x, int src_y, int src_h);

// --- dirty tracking ---
// Dirty tracking is by ROW BAND, not arbitrary rectangles, and always full
// width. That is a hard constraint, not laziness: the panel is an RGB LCD
// configured with psram_trans_align = 64, so a transfer has to start on a
// 64-byte boundary. A full-width row is 480*2 = 960 bytes, and 960 is a
// multiple of 64, so y*960 is always aligned. A dirty rect starting at an odd
// x would not be.
//
// It still pays: a status-bar repaint pushes 480x30 instead of 480x480, and
// an idle screen pushes nothing at all.
void gfx_dirty(int x, int y, int w, int h);
void gfx_dirty_all(void);
// Push the accumulated dirty bands to the panel and clear them. Once a frame.
void gfx_present(void);
