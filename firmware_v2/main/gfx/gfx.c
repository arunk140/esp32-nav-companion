#include "gfx/gfx.h"
#include "display/panel.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "gfx";

gfx_color_t *gfx_fb;

// Dirty state is a pair of row bands. See gfx.h: the panel needs 64-byte
// aligned transfers and a full-width row satisfies that for free, whereas a
// dirty rectangle at an arbitrary x does not.
static int s_dirty_y0, s_dirty_y1;  // empty: y1 < y0
static int s_dirty2_y0, s_dirty2_y1;
static bool s_have_dirty;

void gfx_init(void)
{
    if (gfx_fb) return;
    // PSRAM: the panel DMA reads it, and it is far too big for internal RAM.
    gfx_fb = heap_caps_malloc((size_t)GFX_W * GFX_H * sizeof(gfx_color_t),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!gfx_fb) {
        ESP_LOGE(TAG, "no PSRAM for %dx%d framebuffer", GFX_W, GFX_H);
        abort();
    }
    s_dirty_y1 = -1;  // empty
    s_dirty2_y1 = -1;
    s_have_dirty = false;
    ESP_LOGI(TAG, "framebuffer %dx%d at %p", GFX_W, GFX_H, gfx_fb);
}

void gfx_clear(gfx_color_t c)
{
    // A full-screen fill is the common case (screen switch); memset is much
    // faster than a loop when the colour is a repeated byte pattern.
    uint8_t b = (uint8_t)(c & 0xFF);
    uint8_t b2 = (uint8_t)(c >> 8);
    if (b == b2) {
        memset(gfx_fb, b, (size_t)GFX_W * GFX_H * sizeof(gfx_color_t));
    } else {
        for (int i = 0; i < GFX_W * GFX_H; i++) gfx_fb[i] = c;
    }
    gfx_dirty_all();
}

void gfx_pixel(int x, int y, gfx_color_t c)
{
    if (x < 0 || y < 0 || x >= GFX_W || y >= GFX_H) return;
    gfx_fb[y * GFX_W + x] = c;
}

void gfx_fill_rect(int x, int y, int w, int h, gfx_color_t c)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (x + w) > GFX_W ? GFX_W : (x + w);
    int y1 = (y + h) > GFX_H ? GFX_H : (y + h);
    for (int yy = y0; yy < y1; yy++) {
        gfx_color_t *row = &gfx_fb[yy * GFX_W];
        for (int xx = x0; xx < x1; xx++) row[xx] = c;
    }
    if (x1 > x0 && y1 > y0) gfx_dirty(x0, y0, x1 - x0, y1 - y0);
}

void gfx_rect_outline(int x, int y, int w, int h, gfx_color_t c)
{
    if (w <= 0 || h <= 0) return;
    gfx_fill_rect(x, y, w, 1, c);
    gfx_fill_rect(x, y + h - 1, w, 1, c);
    gfx_fill_rect(x, y + 1, 1, h - 2, c);
    gfx_fill_rect(x + w - 1, y + 1, 1, h - 2, c);
}

// Largest s with s*s + dy*dy <= r*r, by incremental search. Only ever
// evaluated once per row, so the loop is short and branch-predictable.
static int corner_half_span(int r, int dy)
{
    int s = 0;
    while ((s + 1) * (s + 1) + dy * dy <= r * r) s++;
    return s;
}

// Row span of a rounded rect at height j, as [x0, x0+len).
static void round_rect_span(int w, int h, int r, int j, int *x0, int *len)
{
    int dy = 0;
    if (j < r) dy = r - j;
    else if (j >= h - r) dy = j - (h - 1 - r);
    int inset = dy ? r - corner_half_span(r, dy) : 0;
    *x0 = inset;
    *len = w - 2 * inset;
}

void gfx_round_rect(int x, int y, int w, int h, int r, gfx_color_t c)
{
    if (w <= 0 || h <= 0) return;
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r == 0) { gfx_fill_rect(x, y, w, h, c); return; }

    // One span per row, computed from the corner circle. Deriving the shape
    // this way is what makes r == w/2 (a full circle) come out solid: the
    // naive "middle rect + two side rects" decomposition degenerates to zero
    // width at exactly r*2 == w, which is how the buttons ended up as hollow
    // specks.
    for (int j = 0; j < h; j++) {
        int x0, len;
        round_rect_span(w, h, r, j, &x0, &len);
        if (len <= 0) continue;
        for (int i = 0; i < len; i++) gfx_pixel(x + x0 + i, y + j, c);
    }
    gfx_dirty(x, y, w, h);
}

void gfx_round_rect_outline(int x, int y, int w, int h, int r, int t, gfx_color_t c)
{
    if (w <= 0 || h <= 0 || t <= 0) return;
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r == 0) { gfx_rect_outline(x, y, w, h, c); return; }

    // Solid shape, then the interior punched out in the background. Doing it
    // this way keeps the corners correct without a second span computation.
    for (int j = 0; j < h; j++) {
        int x0, len;
        round_rect_span(w, h, r, j, &x0, &len);
        if (len <= 0) continue;
        // Rows near the top/bottom are mostly border; interior rows keep only
        // t pixels at each end.
        if (j >= t && j < h - t) {
            for (int i = 0; i < t && i < len; i++) gfx_pixel(x + x0 + i, y + j, c);
            for (int i = len - t; i < len; i++)
                if (i >= 0) gfx_pixel(x + x0 + i, y + j, c);
        } else {
            for (int i = 0; i < len; i++) gfx_pixel(x + x0 + i, y + j, c);
        }
    }
    gfx_dirty(x, y, w, h);
}

void gfx_circle(int cx, int cy, int r, gfx_color_t c)
{
    if (r <= 0) return;
    int r2 = r * r;
    for (int dy = -r; dy <= r; dy++) {
        int span = (int)(0.5 + sqrt((double)r2 - (double)dy * dy));
        gfx_fill_rect(cx - span, cy + dy, span * 2 + 1, 1, c);
    }
}

// Copy a w*h block to (x,y), clipping to the framebuffer.
//
// src points at the TOP-LEFT OF THE WHOLE SOURCE and src_stride is in pixels,
// so this function derives the source's position in that image itself. It does
// NOT assume src has been pre-offset to the destination origin -- doing that
// and then also skipping clipped rows double-offsets the source, which is
// exactly how the map ended up showing a slice of the wrong part of the
// mosaic.
void gfx_blit_region(int x, int y, int w, int h, const gfx_color_t *src,
                     int src_stride, int src_x, int src_y, int src_h)
{
    if (w <= 0 || h <= 0 || !src) return;

    // Clip the destination to the framebuffer, tracking how many rows and
    // columns of the source that consumes. Every adjustment is expressed as a
    // delta on the SOURCE coordinates so the two can never disagree:
    //   rows:    source row = src_y + (dest row - y)
    //   columns: source col = src_x + (dest col - x)
    int dy = y, ddx = x, rows = h, wpx = w;
    if (dy < 0) { int cut = -dy; if (cut > rows) return; src_y += cut; dy = 0; rows -= cut; }
    if (dy + rows > GFX_H) { rows = GFX_H - dy; if (rows <= 0) return; }
    if (ddx < 0) { int cut = -ddx; if (cut > wpx) return; src_x += cut; ddx = 0; wpx -= cut; }
    if (ddx + wpx > GFX_W) { wpx = GFX_W - ddx; if (wpx <= 0) return; }
    // Refuse to read outside the source. Without this a destination that runs
    // off the end of the source keeps copying, and the surplus lands in
    // whatever follows -- in the map's case, the neighbouring mosaic row,
    // which is how a single bad frame turned into a scrambled screen.
    if (src_x < 0 || src_y < 0 || src_x + wpx > src_stride ||
        src_y + rows > src_h) {
        return;
    }

    for (int r = 0; r < rows; r++) {
        const gfx_color_t *s = &src[(size_t)(src_y + r) * src_stride + src_x];
        memcpy(&gfx_fb[(size_t)(dy + r) * GFX_W + ddx], s, (size_t)wpx * 2);
    }
}

// As above, for a block whose source is already at its own origin.
void gfx_blit_strided(int x, int y, int w, int h, const gfx_color_t *src,
                      int src_stride, int src_h)
{
    gfx_blit_region(x, y, w, h, src, src_stride, 0, 0, src_h);
}

void gfx_blit(int x, int y, int w, int h, const gfx_color_t *src)
{
    gfx_blit_strided(x, y, w, h, src, w, h);
}

void gfx_dirty(int x, int y, int w, int h)
{
    (void)x;
    (void)w;
    int y0 = y < 0 ? 0 : y;
    int y1 = (y + h) > GFX_H ? GFX_H : (y + h);
    if (y1 <= y0) return;
    if (!s_have_dirty) {
        s_dirty_y0 = y0;
        s_dirty_y1 = y1;
        s_dirty2_y1 = -1;
        s_have_dirty = true;
        return;
    }
    if (s_dirty2_y1 < 0) {
        s_dirty2_y0 = y0;
        s_dirty2_y1 = y1;
        return;
    }
    // Both slots taken: coalesce. Pushing a slightly taller band costs little
    // and this keeps the state trivially correct.
    if (y0 < s_dirty_y0) s_dirty_y0 = y0;
    if (y1 > s_dirty_y1) s_dirty_y1 = y1;
    s_dirty2_y1 = -1;
}

void gfx_dirty_all(void)
{
    s_dirty_y0 = 0;
    s_dirty_y1 = GFX_H;
    s_dirty2_y1 = -1;
    s_have_dirty = true;
}

void gfx_present(void)
{
    if (!s_have_dirty) return;
    int b0 = s_dirty_y0, b1 = s_dirty_y1;
    int c0 = s_dirty2_y0, c1 = s_dirty2_y1;
    s_have_dirty = false;
    s_dirty_y1 = -1;
    s_dirty2_y1 = -1;

    for (int pass = 0; pass < 2; pass++) {
        int y0 = pass ? c0 : b0;
        int y1 = pass ? c1 : b1;
        if (y1 <= y0) continue;
        for (int sy = y0; sy < y1; sy += PANEL_STRIP_LINES) {
            int sh = y1 - sy;
            if (sh > PANEL_STRIP_LINES) sh = PANEL_STRIP_LINES;
            // Always full width, and always starting at x=0: see gfx.h for why
            // that is a correctness requirement, not an optimisation.
            panel_present(&gfx_fb[(size_t)sy * GFX_W], 0, sy, GFX_W, sh);
        }
    }
}

// --- font -------------------------------------------------------------------
// 5x7 glyphs, one byte per column, in a 6x8 cell. Uppercase only: the strings
// this UI needs are short and shouty, and a half-size table keeps both the
// flash and the code small.
#define GLYPH_W 5
#define GLYPH_H 7
#define CELL_W 6
#define CELL_H 8

// 'A'..'Z' then '0'..'9' then a few symbols. Bit 0 is the top row.
static const uint8_t FONT[26 + 10 + 10][GLYPH_W] = {
    {0x7E,0x11,0x11,0x11,0x7E}, // A
    {0x7F,0x49,0x49,0x49,0x36}, // B
    {0x3E,0x41,0x41,0x41,0x22}, // C
    {0x7F,0x41,0x41,0x22,0x1C}, // D
    {0x7F,0x49,0x49,0x49,0x41}, // E
    {0x7F,0x09,0x09,0x09,0x01}, // F
    {0x3E,0x41,0x49,0x49,0x7A}, // G
    {0x7F,0x08,0x08,0x08,0x7F}, // H
    {0x00,0x41,0x7F,0x41,0x00}, // I
    {0x20,0x40,0x41,0x3F,0x01}, // J
    {0x7F,0x08,0x14,0x22,0x41}, // K
    {0x7F,0x40,0x40,0x40,0x40}, // L
    {0x7F,0x02,0x0C,0x02,0x7F}, // M
    {0x7F,0x04,0x08,0x10,0x7F}, // N
    {0x3E,0x41,0x41,0x41,0x3E}, // O
    {0x7F,0x09,0x09,0x09,0x06}, // P
    {0x3E,0x41,0x51,0x21,0x5E}, // Q
    {0x7F,0x09,0x19,0x29,0x46}, // R
    {0x46,0x49,0x49,0x49,0x31}, // S
    {0x01,0x01,0x7F,0x01,0x01}, // T
    {0x3F,0x40,0x40,0x40,0x3F}, // U
    {0x1F,0x20,0x40,0x20,0x1F}, // V
    {0x3F,0x40,0x38,0x40,0x3F}, // W
    {0x63,0x14,0x08,0x14,0x63}, // X
    {0x07,0x08,0x70,0x08,0x07}, // Y
    {0x61,0x51,0x49,0x45,0x43}, // Z
    {0x3E,0x51,0x49,0x45,0x3E}, // 0
    {0x00,0x42,0x7F,0x40,0x00}, // 1
    {0x42,0x61,0x51,0x49,0x46}, // 2
    {0x21,0x41,0x45,0x4B,0x31}, // 3
    {0x18,0x14,0x12,0x7F,0x10}, // 4
    {0x27,0x45,0x45,0x45,0x39}, // 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 6
    {0x01,0x71,0x09,0x05,0x03}, // 7
    {0x36,0x49,0x49,0x49,0x36}, // 8
    {0x06,0x49,0x49,0x29,0x1E}, // 9
    {0x00,0x36,0x36,0x00,0x00}, // -
    {0x00,0x00,0x00,0x36,0x36}, // . (comma-ish; use as full stop at baseline)
    {0x00,0x00,0x14,0x08,0x00}, // /
    {0x08,0x08,0x3E,0x08,0x08}, // +
    {0x00,0x00,0x7F,0x00,0x00}, // :
    {0x40,0x40,0x40,0x40,0x40}, // =
    {0x02,0x01,0x51,0x09,0x06}, // ?
    {0x00,0x05,0x03,0x00,0x00}, // ' (apostrophe)
    {0x00,0x00,0x00,0x00,0x00}, // space
    {0x00,0x00,0x00,0x00,0x00}, // (unused)
};

static const uint8_t *glyph_for(char ch)
{
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    if (ch >= 'A' && ch <= 'Z') return FONT[ch - 'A'];
    if (ch >= '0' && ch <= '9') return FONT[26 + (ch - '0')];
    switch (ch) {
    case '-': return FONT[36];
    case '.': case ',': return FONT[37];
    case '/': return FONT[38];
    case '+': return FONT[39];
    case ':': return FONT[40];
    case '=': return FONT[41];
    case '?': return FONT[42];
    case '\'': return FONT[43];
    case ' ': return FONT[44];
    default:  return FONT[45];
    }
}

int gfx_text_width(const char *s, int scale)
{
    int n = 0;
    while (s[n]) n++;
    if (n == 0) return 0;
    return (n * CELL_W - 1) * scale;
}

void gfx_text(int x, int y, const char *s, gfx_color_t c, int scale)
{
    if (scale < 1) scale = 1;
    for (int i = 0; s[i]; i++) {
        const uint8_t *g = glyph_for(s[i]);
        for (int col = 0; col < GLYPH_W; col++) {
            uint8_t bits = g[col];
            for (int row = 0; row < GLYPH_H; row++) {
                if (!(bits & (1 << row))) continue;
                gfx_fill_rect(x + col * scale, y + row * scale, scale, scale, c);
            }
        }
        x += CELL_W * scale;
    }
}

void gfx_text_centred(int cx, int y, const char *s, gfx_color_t c, int scale)
{
    gfx_text(cx - gfx_text_width(s, scale) / 2, y, s, c, scale);
}
