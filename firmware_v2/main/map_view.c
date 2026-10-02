#include "map_view.h"
#include "nav_view.h"
#include "board.h"
#include "display.h"
#include "ui_common.h"
#include "sdcard.h"
#include "settings.h"
#include "tiles.h"
#include "touch.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_LAT 18.9219  // Gateway of India
#define DEFAULT_LON 72.8347
#define DEFAULT_ZOOM 16

// The phone batches fixes up to ~10 s apart under Doze; stale must cover it.
#define STALE_MS 12000
#define SAVE_POS_MS 60000
#define SAVE_ZOOM_MS 3000
#define SNAP_PX 400
#define DRAG_SLOP_PX 8
#define PREFETCH_MS 150
// Heading arrow (and heading-up map in follow mode) above 1 m/s; it only
// goes away below 0.5 m/s so creeping traffic doesn't flip the map around.
#define MOVING_ON_CMS 100
#define MOVING_OFF_CMS 50
#define ROT_RATE 5.0f        // 1/s: rotation eases with a ~200 ms time constant
#define RESET_HOLD_MS 15000
#define RESET_HINT_MS 1500
#define TRIPLE_TAP_N 3
#define TRIPLE_TAP_WINDOW_MS 1000
#define TAP_MAX_MS 500

#define COL_PENDING_L 0xF2EFE9
#define COL_MISSING_L 0xD9DCE0
#define COL_PENDING_D 0x2A2E33
#define COL_MISSING_D 0x14181C
#define COL_LIVE 0x1A73E8
#define COL_STALE 0x8A8F96
#define COL_DEST 0xE8710A
#define COL_GREEN 0x34A853
#define COL_AMBER 0xF9AB00
#define COL_GRAY 0x9AA0A6

#define HALF (BOARD_LCD_W / 2)

// ---- persistent state (survives screen changes) ----
static int s_zmin, s_zmax, s_zoom;
static double s_cx, s_cy;  // view centre, normalized Web-Mercator [0,1)
static bool s_follow = true;
static bool s_has_fix;      // any fix (incl. restored from NVS)
static bool s_fix_live;     // fix received this session
static bool s_was_live;
static bool s_was_nav;
static double s_mx, s_my;   // marker as displayed (animated)
static bool s_has_dest;     // destination marker (from the phone)
static double s_dnx, s_dny; // destination, normalized Web-Mercator
static uint32_t s_line_anim_ms;  // last marching-dots invalidate
static int s_line_phase;         // dot-pattern offset, advanced per frame
static double s_ax, s_ay, s_bx, s_by;
static uint32_t s_anim_t0, s_anim_dur;
static bool s_anim;
static uint32_t s_last_fix_ms;
static uint16_t s_speed_cms, s_cog_cdeg;
static bool s_moving;        // heading arrow shown (with hysteresis)
static float s_rot;          // displayed map rotation, deg clockwise of north-up
static uint32_t s_rot_t;
static int32_t s_fix_lat, s_fix_lon;
static bool s_pos_dirty;
static uint32_t s_pos_saved_ms;
static bool s_zoom_dirty;
static uint32_t s_zoom_changed_ms;
static bool s_connected, s_paired;

// ---- UI objects (valid only while the map screen exists) ----
static lv_obj_t *s_map, *s_pill, *s_pill_dot, *s_pill_lbl;
static lv_obj_t *s_btn_in, *s_btn_out, *s_btn_center, *s_toast;
static uint32_t s_toast_until;
static int s_status_key = -1;
static int s_status_eta_min = -1;   // last ETA minutes drawn (nav pill)
static void (*s_reset_cb)(void);
static void (*s_media_cb)(void);
static void (*s_sleep_cb)(void);
static bool s_sleep_req;
static uint32_t s_last_tap_ms;
static int s_tap_count;
static bool s_reset_req;
static uint32_t s_hold_t0;
static int s_hold_shown = -1;
static bool s_hold_fired;
static uint32_t s_prefetch_ms;

// What the last render showed (to skip redundant redraws).
static int32_t s_r_ox, s_r_oy, s_r_mx, s_r_my;
static bool s_r_valid;

// Gesture state
static lv_point_t s_last_pt;
static int32_t s_drag_acc;
static bool s_dragging, s_pinching, s_pinch_done;
static float s_pinch_d0;

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static double lon_to_nx(double lon)
{
    return (lon + 180.0) / 360.0;
}

static double lat_to_ny(double lat)
{
    if (lat > 85.05) lat = 85.05;
    if (lat < -85.05) lat = -85.05;
    double r = lat * M_PI / 180.0;
    return (1.0 - log(tan(r) + 1.0 / cos(r)) / M_PI) / 2.0;
}

static inline double world_px(void)
{
    return 256.0 * (double)(1u << s_zoom);
}

static inline int32_t floordiv(int32_t a, int32_t b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

static double clamp01(double v)
{
    return v < 0.0 ? 0.0 : (v > 0.999999 ? 0.999999 : v);
}

static void view_origin(int32_t *ox, int32_t *oy)
{
    double w = world_px();
    *ox = (int32_t)floor(s_cx * w) - HALF;
    *oy = (int32_t)floor(s_cy * w) - HALF;
}

static inline bool rotated(float rot)
{
    return fabsf(rot) >= 0.05f;
}

// Normalized world point -> screen for a frame drawn with origin (ox, oy) and
// rotation rot (rotation is about the view centre). Same transform as
// marker_screen, generalised so the destination can use it too.
static void world_screen(double nx, double ny, int32_t ox, int32_t oy, float rot,
                         int32_t *sx, int32_t *sy);

// Marker position on screen for a frame drawn with origin (ox, oy) and
// rotation rot (rotation is about the view centre).
static void marker_screen(int32_t ox, int32_t oy, float rot, int32_t *sx, int32_t *sy)
{
    world_screen(s_mx, s_my, ox, oy, rot, sx, sy);
}

static void world_screen(double nx, double ny, int32_t ox, int32_t oy, float rot,
                         int32_t *sx, int32_t *sy)
{
    double w = world_px();
    if (!rotated(rot)) {
        *sx = (int32_t)floor(nx * w) - ox;
        *sy = (int32_t)floor(ny * w) - oy;
        return;
    }
    float wx = (float)((nx - s_cx) * w), wy = (float)((ny - s_cy) * w);
    float r = rot * (float)M_PI / 180.0f, c = cosf(r), sn = sinf(r);
    *sx = (int32_t)lroundf(239.5f + wx * c + wy * sn);
    *sy = (int32_t)lroundf(239.5f - wx * sn + wy * c);
}

static float wrap180(float a)
{
    while (a > 180.0f) a -= 360.0f;
    while (a <= -180.0f) a += 360.0f;
    return a;
}

static bool fix_is_live(void)
{
    return s_fix_live && (now_ms() - s_last_fix_ms) < STALE_MS;
}

static void invalidate(void)
{
    s_r_valid = false;
    if (s_map) lv_obj_invalidate(s_map);
}

// ---------------------------------------------------------------- render

// Marker geometry, one knob for the lot. Base sizes are in pixels; at 0.8
// the dot is 13 px across with a 29 px halo.
#define MARKER_SCALE 0.6f
#define MARK_HALO_R (24.0f * MARKER_SCALE)
#define MARK_DOT_R (11.0f * MARKER_SCALE)
#define MARK_RING_W 1
#define MARK_CONE_LEN (36.0f * MARKER_SCALE)
#define MARK_CONE_W (15.0f * MARKER_SCALE)

static void draw_marker(lv_layer_t *layer, int32_t x, int32_t y, float rot)
{
    bool live = fix_is_live();
    lv_color_t c = lv_color_hex(live ? COL_LIVE : COL_STALE);
    int32_t hr = (int32_t)lroundf(MARK_HALO_R), dr = (int32_t)lroundf(MARK_DOT_R);

    if (live && s_moving) {
        float a = ((float)s_cog_cdeg / 100.0f - rot) * (float)M_PI / 180.0f;
        lv_draw_triangle_dsc_t t;
        lv_draw_triangle_dsc_init(&t);
        t.color = c;
        t.opa = LV_OPA_70;
        t.p[0].x = x + sinf(a) * MARK_CONE_LEN;
        t.p[0].y = y - cosf(a) * MARK_CONE_LEN;
        t.p[1].x = x + sinf(a - 0.55f) * MARK_CONE_W;
        t.p[1].y = y - cosf(a - 0.55f) * MARK_CONE_W;
        t.p[2].x = x + sinf(a + 0.55f) * MARK_CONE_W;
        t.p[2].y = y - cosf(a + 0.55f) * MARK_CONE_W;
        lv_draw_triangle(layer, &t);
    }

    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.radius = LV_RADIUS_CIRCLE;
    r.bg_color = c;
    r.bg_opa = LV_OPA_20;
    lv_area_t halo = {x - hr, y - hr, x + hr, y + hr};
    lv_draw_rect(layer, &r, &halo);

    r.bg_opa = LV_OPA_COVER;
    r.border_color = lv_color_white();
    r.border_width = MARK_RING_W;
    r.border_opa = LV_OPA_COVER;
    lv_area_t dot = {x - dr, y - dr, x + dr, y + dr};
    lv_draw_rect(layer, &r, &dot);
}

#define DEST_R 7

// Destination: a plain filled orange dot. Ring follows the theme so it reads on
// both a dark and a light map.
static void draw_dest(lv_layer_t *layer, int32_t x, int32_t y)
{
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.radius = LV_RADIUS_CIRCLE;
    r.bg_color = lv_color_hex(COL_DEST);
    r.bg_opa = LV_OPA_COVER;
    r.border_color = lv_color_hex(tiles_dark() ? 0x000000 : 0xFFFFFF);
    r.border_width = 2;
    r.border_opa = LV_OPA_60;
    lv_area_t disc = {x - DEST_R, y - DEST_R, x + DEST_R, y + DEST_R};
    lv_draw_rect(layer, &r, &disc);
}

// Dotted line from the position marker to the destination. `phase` shifts the
// dash start so the dots march toward the destination.
//   LINK_STEP    px the pattern advances per rendered frame (uniform, so motion
//                is smooth regardless of when a frame happens to draw)
//   LINK_ANIM_MS how often we ask for a frame; period/step frames = one cycle
#define LINK_DASH 5
#define LINK_GAP 6
#define LINK_W 3
#define LINK_STEP 1
#define LINK_ANIM_MS 220   // ~4.5 fps: calm march, one cycle in ~3.5 s

// White on a dark map, light grey on a light one.
#define LINK_COL_DARK 0xFFFFFF
#define LINK_COL_LIGHT 0xB0B4B8

static void draw_dest_line(lv_layer_t *layer, int32_t x0, int32_t y0,
                           int32_t x1, int32_t y1, int32_t phase)
{
    float dx = (float)(x1 - x0), dy = (float)(y1 - y0);
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 4.0f) return;
    float ux = dx / len, uy = dy / len;

    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LINK_W;              // capsule caps
    d.bg_color = lv_color_hex(tiles_dark() ? LINK_COL_DARK : LINK_COL_LIGHT);
    d.bg_opa = LV_OPA_70;
    d.border_opa = LV_OPA_TRANSP;

    const int period = LINK_DASH + LINK_GAP;
    int32_t p = phase % period;
    for (float s = (float)p - period; s < len; s += period) {
        float a = s < 0 ? 0 : s;
        float b = s + LINK_DASH;
        if (a >= len) break;
        if (b > len) b = len;
        if (b - a < 1.0f) continue;
        int32_t ax = (int32_t)lroundf(x0 + ux * a), ay = (int32_t)lroundf(y0 + uy * a);
        int32_t bx = (int32_t)lroundf(x0 + ux * b), by = (int32_t)lroundf(y0 + uy * b);
        lv_area_t e = {
            .x1 = (ax < bx ? ax : bx) - LINK_W / 2,
            .y1 = (ay < by ? ay : by) - LINK_W / 2,
            .x2 = (ax < bx ? bx : ax) + LINK_W / 2,
            .y2 = (ay < by ? by : ay) + LINK_W / 2,
        };
        lv_draw_rect(layer, &d, &e);
    }
}

typedef struct {
    int32_t tx, ty;
    int d;
} vis_tile_t;

// Origin + rotation used by the frame being rendered (blit + marker agree).
static int32_t s_f_ox, s_f_oy;
static float s_f_rot;
static int32_t s_r_rot;  // rotation of the last render, 0.1 deg units

// Placeholder colour for tiles that are not drawable yet. Follows the theme
// so a dark map isn't speckled with light blocks.
static uint32_t fill_colour(tile_state_t st)
{
    bool dark = tiles_dark();
    return dark ? (st == TILE_MISSING ? COL_MISSING_D : COL_PENDING_D)
                : (st == TILE_MISSING ? COL_MISSING_L : COL_PENDING_L);
}

static void fill_rect(uint8_t *fb, uint32_t stride, int x0, int y0, int w, int h, uint16_t c)
{
    static uint16_t row[BOARD_LCD_W];
    static uint16_t row_c = 0;
    static bool row_ok;
    if (!row_ok || row_c != c) {
        for (int i = 0; i < BOARD_LCD_W; i++) row[i] = c;
        row_c = c;
        row_ok = true;
    }
    for (int y = y0; y < y0 + h; y++) memcpy(fb + y * stride + x0 * 2, row, w * 2);
}

// ---- heading-up rendering ----
// Only the visible disc (r = 240) is drawn: the panel is round, the corners
// don't exist. Each screen row is a straight line through the tile grid, so
// it is walked with constant 16.16 fixed-point steps (no trig per pixel),
// bilinear-filtered so rotated roads and labels stay smooth.

#define RG 4  // a disc of diameter 480 + margin touches at most 3x3 tiles

typedef struct {
    const uint16_t *t[RG][RG];
    uint16_t fill[RG][RG];
} rgrid_t;

// Bilinear filtering. An RGB565 pixel spread into 0x07E0F81F (G | R | B with
// >= 5 spare bits above each channel) lets all three channels be weighted at
// once: four weights summing to 16 need only 4 spare bits, so a whole 2x2
// blend is four multiplies with no overflow.
#define SPREAD_MASK 0x07E0F81Fu

static inline uint32_t spread565(uint16_t p)
{
    return ((uint32_t)p | ((uint32_t)p << 16)) & SPREAD_MASK;
}

// Block-local bilinear span: samples a staged copy of the block's source
// rectangle in internal SRAM (stride ST). The stage always includes the +1
// neighbours, so there are no bounds or tile checks in here.
enum { ST = 64 };

static void IRAM_ATTR rot_span(uint16_t *dst, int n, int32_t u, int32_t v, int32_t du, int32_t dv,
                               const uint16_t *stage)
{
    for (int i = 0; i < n; i++) {
        const uint16_t *p = stage + ((uint32_t)v >> 16) * ST + ((uint32_t)u >> 16);
        // 4-bit sub-texel position -> four weights summing to 16.
        uint32_t fx = ((uint32_t)u >> 12) & 15, fy = ((uint32_t)v >> 12) & 15;
        uint32_t wd = (fx * fy) >> 4;
        uint32_t wb = fx - wd, wc = fy - wd, wa = 16 - fx - fy + wd;
        uint32_t m = (spread565(p[0]) * wa + spread565(p[1]) * wb + spread565(p[ST]) * wc +
                      spread565(p[ST + 1]) * wd) >> 4;
        m &= SPREAD_MASK;
        dst[i] = (uint16_t)(m | (m >> 16));
        u += du;
        v += dv;
    }
}

// Copies texels [u0, u0 + w) of world row v (grid-relative) into dst.
static void stage_row(uint16_t *dst, int32_t u0, int32_t v, int w, const rgrid_t *g)
{
    int32_t gy = floordiv(v, TILE_PX), ty = v - gy * TILE_PX;
    int x = 0;
    while (x < w) {
        int32_t u = u0 + x;
        int32_t gx = floordiv(u, TILE_PX), tx = u - gx * TILE_PX;
        int n = w - x < TILE_PX - tx ? w - x : TILE_PX - tx;
        if (gx >= 0 && gx < RG && gy >= 0 && gy < RG && g->t[gy][gx]) {
            memcpy(dst + x, g->t[gy][gx] + ty * TILE_PX + tx, n * 2);
        } else {
            uint16_t f = (gx >= 0 && gx < RG && gy >= 0 && gy < RG) ? g->fill[gy][gx] : 0;
            for (int k = 0; k < n; k++) dst[x + k] = f;
        }
        x += n;
    }
}

// Rotated frame job, shared by the GUI task (core 1) and a helper (core 0).
// Rendered in BLK x BLK blocks. For each block the source rectangle it
// samples (<= 49x49 texels at 45 deg) is first copied into internal SRAM with
// row memcpys (sequential PSRAM bursts), then bilinear-sampled from there:
// sampling straight from PSRAM stalls on cache misses (two source rows per
// pixel, shared bus with the LCD scan-out).
enum { BLK = 32 };

typedef struct {
    uint8_t *fb;
    uint32_t stride;
    rgrid_t g;
    float c, sn, ocx, ocy;
    int32_t du, dv;
    int next;  // next band's first row, under s_job_mux
} rjob_t;

static rjob_t s_job;
static portMUX_TYPE s_job_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_job_go, s_job_done;
static TaskHandle_t s_helper;
static int16_t s_xs_tab[BOARD_LCD_H], s_xe_tab[BOARD_LCD_H];
// Per-core staging buffers (internal SRAM): [0] GUI task, [1] helper.
static uint16_t s_stage[2][ST * ST];

static void rot_block(int bx, int by, uint16_t *stage)
{
    const rjob_t *j = &s_job;
    // Rows of this block that intersect the disc.
    int y0 = by, y1 = by + BLK - 1;
    while (y0 <= y1 && (s_xe_tab[y0] < bx || s_xs_tab[y0] > bx + BLK - 1)) y0++;
    while (y1 >= y0 && (s_xe_tab[y1] < bx || s_xs_tab[y1] > bx + BLK - 1)) y1--;
    if (y0 > y1) return;

    // Source rectangle covered by the block (corners), plus bilinear margin.
    float umin = 1e9f, umax = -1e9f, vmin = 1e9f, vmax = -1e9f;
    for (int k = 0; k < 4; k++) {
        float px = (k & 1 ? bx + BLK - 1 : bx) - 239.5f;
        float py = (k & 2 ? y1 : y0) - 239.5f;
        float u = j->ocx + px * j->c - py * j->sn - 0.5f;
        float v = j->ocy + px * j->sn + py * j->c - 0.5f;
        umin = fminf(umin, u), umax = fmaxf(umax, u);
        vmin = fminf(vmin, v), vmax = fmaxf(vmax, v);
    }
    int32_t iu0 = (int32_t)floorf(umin) - 1, iv0 = (int32_t)floorf(vmin) - 1;
    int w = (int32_t)floorf(umax) + 3 - iu0, h = (int32_t)floorf(vmax) + 3 - iv0;
    if (w > ST) w = ST;
    if (h > ST) h = ST;
    for (int r = 0; r < h; r++) stage_row(stage + r * ST, iu0, iv0 + r, w, &j->g);

    for (int y = y0; y <= y1; y++) {
        int xs = s_xs_tab[y] > bx ? s_xs_tab[y] : bx;
        int xe = s_xe_tab[y] < bx + BLK - 1 ? s_xe_tab[y] : bx + BLK - 1;
        if (xe < xs) continue;
        float px = xs - 239.5f, py = y - 239.5f;
        // -0.5: bilinear weights are relative to texel centres.
        float u = j->ocx + px * j->c - py * j->sn - 0.5f - iu0;
        float v = j->ocy + px * j->sn + py * j->c - 0.5f - iv0;
        rot_span((uint16_t *)(j->fb + y * j->stride) + xs, xe - xs + 1, (int32_t)(u * 65536.0f),
                 (int32_t)(v * 65536.0f), j->du, j->dv, stage);
    }
}

static void rot_band(int by, uint16_t *stage)
{
    for (int bx = 0; bx < BOARD_LCD_W; bx += BLK) rot_block(bx, by, stage);
}

static void rot_work(uint16_t *stage)
{
    for (;;) {
        taskENTER_CRITICAL(&s_job_mux);
        int by = s_job.next;
        s_job.next += BLK;
        taskEXIT_CRITICAL(&s_job_mux);
        if (by >= BOARD_LCD_H) return;
        rot_band(by, stage);
    }
}

static void rot_helper_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_job_go, portMAX_DELAY);
        rot_work(s_stage[1]);
        xSemaphoreGive(s_job_done);
    }
}

static void rot_init(void)
{
    for (int y = 0; y < BOARD_LCD_H; y++) {
        float py = y - 239.5f, h2 = 240.0f * 240.0f - py * py;
        float hw = h2 > 0.0f ? sqrtf(h2) : 0.0f;
        int xs = (int)ceilf(239.5f - hw), xe = (int)floorf(239.5f + hw);
        s_xs_tab[y] = xs < 0 ? 0 : xs;
        s_xe_tab[y] = xe > BOARD_LCD_W - 1 ? BOARD_LCD_W - 1 : xe;
    }
    s_job_go = xSemaphoreCreateBinary();
    s_job_done = xSemaphoreCreateBinary();
    // Below the tile loader (3): SD reads are short bursts between DMA waits
    // and must never queue behind a frame. Album art decode runs at 1.
    if (xTaskCreatePinnedToCore(rot_helper_task, "rot", 3072, NULL, 2, &s_helper, 0) != pdPASS) {
        s_helper = NULL;  // single-core fallback: GUI task renders it all
    }
}

static void render_rotated(uint8_t *fb, uint32_t stride, float rot)
{
    double w = world_px();
    double cxw = s_cx * w, cyw = s_cy * w;
    int32_t icx = (int32_t)floor(cxw), icy = (int32_t)floor(cyw);
    int32_t gx0 = floordiv(icx - 242, TILE_PX), gx1 = floordiv(icx + 242, TILE_PX);
    int32_t gy0 = floordiv(icy - 242, TILE_PX), gy1 = floordiv(icy + 242, TILE_PX);
    if (gx1 - gx0 >= RG) gx1 = gx0 + RG - 1;
    if (gy1 - gy0 >= RG) gy1 = gy0 + RG - 1;
    int32_t ctx = floordiv(icx, TILE_PX), cty = floordiv(icy, TILE_PX);

    vis_tile_t v[RG * RG];
    int n = 0;
    for (int32_t ty = gy0; ty <= gy1; ty++) {
        for (int32_t tx = gx0; tx <= gx1; tx++) {
            v[n++] = (vis_tile_t){tx, ty, abs(tx - ctx) + abs(ty - cty)};
        }
    }
    for (int i = 1; i < n; i++) {  // farthest first, as in the fast path
        vis_tile_t k = v[i];
        int j = i - 1;
        while (j >= 0 && v[j].d < k.d) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = k;
    }
    rgrid_t g = {0};
    for (int i = 0; i < n; i++) {
        tile_state_t st;
        const lv_image_dsc_t *img = tiles_get(s_zoom, v[i].tx, v[i].ty, true, &st);
        int gx = v[i].tx - gx0, gy = v[i].ty - gy0;
        g.t[gy][gx] = img ? (const uint16_t *)img->data : NULL;
        g.fill[gy][gx] = lv_color_to_u16(lv_color_hex(fill_colour(st)));
    }

    // Screen offset (px, py) from the centre maps to world offset R(rot)*(px, py).
    float r = rot * (float)M_PI / 180.0f;
    s_job.fb = fb;
    s_job.stride = stride;
    s_job.g = g;
    s_job.c = cosf(r);
    s_job.sn = sinf(r);
    s_job.ocx = (float)(cxw - gx0 * (double)TILE_PX);
    s_job.ocy = (float)(cyw - gy0 * (double)TILE_PX);
    s_job.du = (int32_t)lroundf(s_job.c * 65536.0f);
    s_job.dv = (int32_t)lroundf(s_job.sn * 65536.0f);
    s_job.next = 0;
    // Both cores pull bands until the frame is done (core 0 also serves the
    // LCD ISR, so a shared counter balances better than a fixed split).
    if (s_helper) xSemaphoreGive(s_job_go);
    rot_work(s_stage[0]);
    if (s_helper) xSemaphoreTake(s_job_done, pdMS_TO_TICKS(200));
}

// Paints the whole map straight into the frame buffer LVGL is about to
// render. Tight row memcpy from the tile cache: no LVGL image pipeline, no
// background fill underneath. LVGL then draws the marker + widgets on top.
static void render_start_cb(lv_event_t *e)
{
    lv_display_t *disp = lv_event_get_target(e);
    lv_draw_buf_t *buf = lv_display_get_buf_active(disp);
    if (!s_map || !buf) return;
    uint8_t *fb = buf->data;
    uint32_t stride = buf->header.stride;

    int32_t ox, oy;
    view_origin(&ox, &oy);
    s_f_ox = ox;
    s_f_oy = oy;
    s_f_rot = s_rot;
    tiles_begin_frame();
    if (rotated(s_f_rot)) {
        render_rotated(fb, stride, s_f_rot);
        return;
    }

    int32_t tx0 = floordiv(ox, TILE_PX), tx1 = floordiv(ox + BOARD_LCD_W - 1, TILE_PX);
    int32_t ty0 = floordiv(oy, TILE_PX), ty1 = floordiv(oy + BOARD_LCD_H - 1, TILE_PX);
    int32_t ctx = floordiv(ox + HALF, TILE_PX), cty = floordiv(oy + HALF, TILE_PX);

    vis_tile_t v[16];
    int n = 0;
    for (int32_t ty = ty0; ty <= ty1 && n < 16; ty++) {
        for (int32_t tx = tx0; tx <= tx1 && n < 16; tx++) {
            v[n++] = (vis_tile_t){tx, ty, abs(tx - ctx) + abs(ty - cty)};
        }
    }
    // Farthest first: urgent loads go to the queue front, so the centre
    // tile ends up being read first.
    for (int i = 1; i < n; i++) {
        vis_tile_t k = v[i];
        int j = i - 1;
        while (j >= 0 && v[j].d < k.d) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = k;
    }

    for (int i = 0; i < n; i++) {
        int32_t sx = v[i].tx * TILE_PX - ox, sy = v[i].ty * TILE_PX - oy;
        int x0 = sx < 0 ? 0 : sx, y0 = sy < 0 ? 0 : sy;
        int x1 = sx + TILE_PX > BOARD_LCD_W ? BOARD_LCD_W : sx + TILE_PX;
        int y1 = sy + TILE_PX > BOARD_LCD_H ? BOARD_LCD_H : sy + TILE_PX;
        if (x1 <= x0 || y1 <= y0) continue;
        tile_state_t st;
        const lv_image_dsc_t *img = tiles_get(s_zoom, v[i].tx, v[i].ty, true, &st);
        if (img) {
            const uint8_t *src = img->data + (y0 - sy) * (TILE_PX * 2) + (x0 - sx) * 2;
            uint8_t *dst = fb + y0 * stride + x0 * 2;
            size_t len = (size_t)(x1 - x0) * 2;
            for (int y = y0; y < y1; y++) {
                memcpy(dst, src, len);
                dst += stride;
                src += TILE_PX * 2;
            }
        } else {
            fill_rect(fb, stride, x0, y0, x1 - x0, y1 - y0,
                      lv_color_to_u16(lv_color_hex(fill_colour(st))));
        }
    }
}

static void draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    int32_t sx = 0, sy = 0;
    bool have_sx = false;
    if (s_has_fix) {
        marker_screen(s_f_ox, s_f_oy, s_f_rot, &sx, &sy);
        have_sx = true;
    }
    // Suggested route: a marching dotted line from us to the destination,
    // drawn first so both markers sit on top. The phase advances one step per
    // rendered frame (not wall-clock): tying it to now_ms() made the pattern
    // jitter, because draw timing is independent of the tick that invalidates.
    if (s_has_dest && have_sx) {
        int32_t dx, dy;
        world_screen(s_dnx, s_dny, s_f_ox, s_f_oy, s_f_rot, &dx, &dy);
        s_line_phase += LINK_STEP;
        if (s_line_phase >= LINK_DASH + LINK_GAP) s_line_phase -= LINK_DASH + LINK_GAP;
        draw_dest_line(layer, sx, sy, dx, dy, s_line_phase);
    }
    if (s_has_dest) {
        int32_t dx, dy;
        world_screen(s_dnx, s_dny, s_f_ox, s_f_oy, s_f_rot, &dx, &dy);
        if (dx > -40 && dx < BOARD_LCD_W + 40 && dy > -40 && dy < BOARD_LCD_H + 40) {
            draw_dest(layer, dx, dy);
        }
    }
    if (have_sx && sx > -40 && sx < BOARD_LCD_W + 40 && sy > -40 && sy < BOARD_LCD_H + 40) {
        draw_marker(layer, sx, sy, s_f_rot);
    }
    s_r_rot = lroundf(s_f_rot * 10.0f);
    s_r_ox = s_f_ox;
    s_r_oy = s_f_oy;
    s_r_mx = sx;
    s_r_my = sy;
    s_r_valid = true;
}

static void cover_check_cb(lv_event_t *e)
{
    // We paint every pixel (render_start_cb), so LVGL must not draw
    // anything underneath. Runs before the class handler, which would
    // otherwise report NOT_COVER for a style-less object.
    lv_event_set_cover_res(e, LV_COVER_RES_COVER);
    lv_event_stop_processing(e);
}

static void prefetch_ring(void)
{
    int32_t ox, oy;
    view_origin(&ox, &oy);
    int32_t tx0 = floordiv(ox, TILE_PX) - 1, tx1 = floordiv(ox + BOARD_LCD_W - 1, TILE_PX) + 1;
    int32_t ty0 = floordiv(oy, TILE_PX) - 1, ty1 = floordiv(oy + BOARD_LCD_H - 1, TILE_PX) + 1;
    for (int32_t ty = ty0; ty <= ty1; ty++) {
        for (int32_t tx = tx0; tx <= tx1; tx++) {
            if (ty == ty0 || ty == ty1 || tx == tx0 || tx == tx1) tiles_prefetch(s_zoom, tx, ty);
        }
    }
}

// ---------------------------------------------------------------- overlays

static void update_status(void)
{
    if (!s_pill) return;
    int key;
    const char *txt;
    uint32_t col;
    bool live = fix_is_live();
    if (!s_paired) {
        key = 0, txt = "Not paired", col = COL_GRAY;
    } else if (!s_connected) {
        key = 1, txt = "Searching for phone", col = COL_AMBER;
    } else if (nav_view_active()) {
        // Navigation owns the pill: ETA from the remaining time the phone
        // already sends (NAV_STATE.remaining_s). Back to "Live" when it ends.
        key = 5, txt = NULL, col = COL_LIVE;
    } else if (live) {
        key = 2, txt = "Live", col = COL_GREEN;
    } else if (s_fix_live) {
        key = 3, txt = "GPS signal lost", col = COL_AMBER;
    } else {
        key = 4, txt = "Waiting for GPS", col = COL_AMBER;
    }

    char eta[24];
    int eta_min = -1;
    if (key == 5) {
        uint16_t rs = nav_view_remaining_s();
        // Round up: "1m" until the last minute, then "<1m".
        eta_min = (rs + 30) / 60;
        if (rs < 60) snprintf(eta, sizeof(eta), "ETA <1m");
        else snprintf(eta, sizeof(eta), "ETA %dm", eta_min);
        txt = eta;
    }

    // Re-render when the base state changes, or when the ETA minute changes.
    if (key == s_status_key && eta_min == s_status_eta_min) return;
    s_status_key = key;
    s_status_eta_min = eta_min;
    lv_label_set_text(s_pill_lbl, txt);
    lv_obj_set_style_bg_color(s_pill_dot, lv_color_hex(col), 0);
}

static void update_buttons(void)
{
    if (!s_map) return;
    if (s_zoom >= s_zmax) {
        lv_obj_add_state(s_btn_in, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(s_btn_in, LV_STATE_DISABLED);
    }
    if (s_zoom <= s_zmin) {
        lv_obj_add_state(s_btn_out, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(s_btn_out, LV_STATE_DISABLED);
    }
    // Recentre only matters while browsing; during navigation the banner owns
    // that area of the screen.
    lv_obj_set_hidden(s_btn_center, s_follow || nav_view_active());
}

static void show_toast(const char *txt)
{
    if (!s_toast) return;
    lv_label_set_text(s_toast, txt);
    lv_obj_set_hidden(s_toast, false);
    s_toast_until = now_ms() + 1200;
}

// ---------------------------------------------------------------- zoom

static void apply_zoom(int z, int ax, int ay)
{
    if (z < s_zmin) z = s_zmin;
    if (z > s_zmax) z = s_zmax;
    if (z == s_zoom) return;
    if (!s_follow) {
        // Keep the point under the anchor (pinch centre) fixed on screen.
        // Anchor offset in world axes (the map may still be rotating back).
        float r = s_rot * (float)M_PI / 180.0f, c = cosf(r), sn = sinf(r);
        double offx = (ax - HALF) * c - (ay - HALF) * sn;
        double offy = (ax - HALF) * sn + (ay - HALF) * c;
        double w0 = world_px();
        double nx = s_cx + offx / w0, ny = s_cy + offy / w0;
        s_zoom = z;
        double w1 = world_px();
        s_cx = clamp01(nx - offx / w1);
        s_cy = clamp01(ny - offy / w1);
    } else {
        s_zoom = z;
    }
    tiles_set_active_zoom(z);
    s_zoom_dirty = true;
    s_zoom_changed_ms = now_ms();
    update_buttons();
    invalidate();
}

void map_view_set_zoom(int z)
{
    apply_zoom(z, HALF, HALF);
}

void map_view_recenter(void)
{
    s_follow = true;
    if (s_has_fix) {
        s_cx = s_mx;
        s_cy = s_my;
    } else {
        s_cx = lon_to_nx(DEFAULT_LON);
        s_cy = lat_to_ny(DEFAULT_LAT);
    }
    update_buttons();
    invalidate();
}

// ---------------------------------------------------------------- input

static void map_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;

    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(indev, &s_last_pt);
        s_drag_acc = 0;
        s_dragging = s_pinching = s_pinch_done = false;
    } else if (code == LV_EVENT_PRESSING) {
        const touch_sample_t *t = touch_get();
        if (t->n >= 2) {
            float dx = t->x[1] - t->x[0], dy = t->y[1] - t->y[0];
            float d = sqrtf(dx * dx + dy * dy);
            if (!s_pinching) {
                s_pinching = true;
                s_pinch_d0 = d > 30.0f ? d : 30.0f;
            } else if (!s_pinch_done) {
                float r = d / s_pinch_d0;
                int mx = (t->x[0] + t->x[1]) / 2, my = (t->y[0] + t->y[1]) / 2;
                if (r > 1.45f) {
                    apply_zoom(s_zoom + 1, mx, my);
                    s_pinch_done = true;
                } else if (r < 0.69f) {
                    apply_zoom(s_zoom - 1, mx, my);
                    s_pinch_done = true;
                }
            }
            return;
        }
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        int32_t dx = p.x - s_last_pt.x, dy = p.y - s_last_pt.y;
        s_last_pt = p;
        if (s_pinching) return;  // one finger left after a pinch: ignore
        if (!s_dragging) {
            s_drag_acc += abs(dx) + abs(dy);
            if (s_drag_acc < DRAG_SLOP_PX) return;
            s_dragging = true;
            s_follow = false;
            update_buttons();
        }
        // Screen drag -> world axes (the map may still be rotating back).
        double w = world_px();
        float r = s_rot * (float)M_PI / 180.0f, c = cosf(r), sn = sinf(r);
        s_cx = clamp01(s_cx - (dx * c - dy * sn) / w);
        s_cy = clamp01(s_cy - (dx * sn + dy * c) / w);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_dragging = s_pinching = false;
    }
}

static void zoom_in_cb(lv_event_t *e)
{
    (void)e;
    apply_zoom(s_zoom + 1, HALF, HALF);
}

static void zoom_out_cb(lv_event_t *e)
{
    (void)e;
    apply_zoom(s_zoom - 1, HALF, HALF);
}

static void center_cb(lv_event_t *e)
{
    (void)e;
    map_view_recenter();
}

static void media_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_media_cb) s_media_cb();
}

// Status pill: hold 15 s to reset pairing. The reset itself is deferred to
// map_view_tick: it rebuilds the screen, deleting this pill, which must not
// happen inside the pill's own event callback.
static void pill_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    uint32_t t = now_ms();
    if (code == LV_EVENT_PRESSED) {
        s_hold_t0 = t;
        s_hold_shown = -1;
        s_hold_fired = false;
    } else if (code == LV_EVENT_PRESSING) {
        if (s_hold_fired || !s_hold_t0) return;
        uint32_t held = t - s_hold_t0;
        if (held >= RESET_HOLD_MS) {
            s_hold_fired = true;
            s_reset_req = true;
            return;
        }
        if (held >= RESET_HINT_MS) {
            int left = (int)((RESET_HOLD_MS - held + 999) / 1000);
            if (left != s_hold_shown) {
                s_hold_shown = left;
                char buf[48];
                lv_snprintf(buf, sizeof(buf), "Keep holding to reset pairing\n%d s", left);
                show_toast(buf);
                s_toast_until = t + 1500;  // refreshed every second while held
            }
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        // A press shorter than TAP_MAX_MS is a tap; anything longer is the
        // start of the hold, and cancels tap counting.
        if (s_hold_t0 && t - s_hold_t0 <= TAP_MAX_MS && !s_hold_fired) {
            if (s_last_tap_ms && t - s_last_tap_ms > TRIPLE_TAP_WINDOW_MS) s_tap_count = 0;
            s_last_tap_ms = t;
            if (++s_tap_count >= TRIPLE_TAP_N) {
                s_tap_count = 0;
                s_sleep_req = true;
            } else if (s_tap_count == TRIPLE_TAP_N - 1) {
                show_toast("Tap again to sleep");
            }
        } else {
            s_tap_count = 0;
            if (!s_hold_fired && s_hold_shown >= 0) show_toast("Reset cancelled");
        }
        s_hold_t0 = 0;
        s_hold_shown = -1;
    }
}

void map_view_invalidate(void)
{
    invalidate();
}

void map_view_toast(const char *txt)
{
    if (!s_toast) return;  // not on the map screen
    show_toast(txt);
}

void map_view_set_reset_cb(void (*cb)(void))
{
    s_reset_cb = cb;
}

void map_view_set_media_cb(void (*cb)(void))
{
    s_media_cb = cb;
}

void map_view_set_sleep_cb(void (*cb)(void))
{
    s_sleep_cb = cb;
}

// ---------------------------------------------------------------- lifecycle

void map_view_init_state(void)
{
    rot_init();
    s_zmin = tiles_zoom_min();
    s_zmax = tiles_zoom_max();
    if (!s_zmin) {
        s_zmin = 15;
        s_zmax = 17;
    }
    int z = settings_zoom();
    if (z < s_zmin || z > s_zmax) z = DEFAULT_ZOOM;
    if (z < s_zmin) z = s_zmin;
    if (z > s_zmax) z = s_zmax;
    s_zoom = z;
    tiles_set_active_zoom(z);

    int32_t la, lo;
    if (settings_last_pos(&la, &lo) && (la || lo)) {
        s_mx = s_bx = lon_to_nx(lo / 1e7);
        s_my = s_by = lat_to_ny(la / 1e7);
        s_has_fix = true;  // shown grey until a live fix arrives
        s_fix_lat = la;
        s_fix_lon = lo;
    }
    map_view_recenter();
}

void map_view_create(lv_obj_t *parent)
{
    lv_obj_set_style_bg_opa(parent, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollable(parent, false);

    s_map = lv_obj_create(parent);
    lv_obj_remove_style_all(s_map);
    lv_obj_set_size(s_map, BOARD_LCD_W, BOARD_LCD_H);
    lv_obj_set_clickable(s_map, true);
    lv_obj_set_scrollable(s_map, false);
    lv_obj_set_gesture_bubble(s_map, false);
    lv_obj_set_scroll_chain(s_map, false);
    lv_obj_set_scroll_on_focus(s_map, false);
    lv_obj_add_event_cb(s_map, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(s_map, cover_check_cb, LV_EVENT_COVER_CHECK | LV_EVENT_PREPROCESS, NULL);
    lv_obj_add_event_cb(s_map, map_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_map, map_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_map, map_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_map, map_event_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_display_add_event_cb(display_get(), render_start_cb, LV_EVENT_RENDER_START, NULL);

    // Status pill, top centre (inside the round safe area).
    s_pill = lv_obj_create(parent);
    lv_obj_remove_style_all(s_pill);
    lv_obj_set_size(s_pill, LV_SIZE_CONTENT, 34);
    lv_obj_align(s_pill, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_set_style_bg_color(s_pill, lv_color_hex(0x202124), 0);
    lv_obj_set_style_bg_opa(s_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_pill, 17, 0);
    lv_obj_set_style_pad_hor(s_pill, 14, 0);
    lv_obj_set_flex_flow(s_pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_pill, 8, 0);
    lv_obj_set_scrollable(s_pill, false);
    lv_obj_set_clickable(s_pill, true);
    lv_obj_set_ext_click_area(s_pill, 14);
    lv_obj_set_style_bg_color(s_pill, lv_color_hex(0x3C4043), LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_pill, pill_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_pill, pill_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_pill, pill_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_pill, pill_event_cb, LV_EVENT_PRESS_LOST, NULL);
    s_pill_dot = lv_obj_create(s_pill);
    lv_obj_remove_style_all(s_pill_dot);
    lv_obj_set_size(s_pill_dot, 10, 10);
    lv_obj_set_style_radius(s_pill_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_pill_dot, LV_OPA_COVER, 0);
    lv_obj_set_clickable(s_pill_dot, false);
    s_pill_lbl = lv_label_create(s_pill);
    lv_obj_set_style_text_font(s_pill_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_pill_lbl, lv_color_white(), 0);
    s_status_key = -1;
    s_status_eta_min = -1;
    update_status();

    const lv_font_t *f = &lv_font_montserrat_28;
    s_btn_in = ui_round_button(parent, LV_SYMBOL_PLUS, 410, 200, 58, f, zoom_in_cb);
    s_btn_out = ui_round_button(parent, LV_SYMBOL_MINUS, 410, 280, 58, f, zoom_out_cb);
    s_btn_center = ui_round_button(parent, LV_SYMBOL_GPS, HALF, 410, 58, f, center_cb);
    ui_round_button(parent, LV_SYMBOL_AUDIO, 70, 240, 58, f, media_btn_cb);

    s_toast = lv_label_create(parent);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x202124), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_80, 0);
    lv_obj_set_style_text_color(s_toast, lv_color_white(), 0);
    lv_obj_set_style_pad_hor(s_toast, 14, 0);
    lv_obj_set_style_pad_ver(s_toast, 6, 0);
    lv_obj_set_style_radius(s_toast, 14, 0);
    lv_obj_set_style_text_align(s_toast, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_toast, LV_ALIGN_TOP_MID, 0, 66);
    lv_obj_set_hidden(s_toast, true);

    if (!tiles_zoom_min()) {
        lv_obj_t *msg = lv_label_create(parent);
        lv_label_set_text(msg, sdcard_ok() ? "No map tiles found in\n/sdcard/street_map"
                                           : "No SD card\nInsert the map card and restart");
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_bg_color(msg, lv_color_hex(0x202124), 0);
        lv_obj_set_style_bg_opa(msg, LV_OPA_90, 0);
        lv_obj_set_style_text_color(msg, lv_color_white(), 0);
        lv_obj_set_style_pad_all(msg, 14, 0);
        lv_obj_set_style_radius(msg, 12, 0);
        lv_obj_center(msg);
    }

    update_buttons();
    invalidate();
}

void map_view_destroy(void)
{
    s_hold_t0 = 0;
    s_hold_shown = -1;
    s_tap_count = 0;
    if (s_map) lv_display_remove_event_cb_with_user_data(display_get(), render_start_cb, NULL);
    // Objects are deleted by the caller cleaning the screen.
    s_map = s_pill = s_pill_dot = s_pill_lbl = NULL;
    s_btn_in = s_btn_out = s_btn_center = s_toast = NULL;
    s_r_valid = false;
}

bool map_view_active(void)
{
    return s_map != NULL;
}

// ---------------------------------------------------------------- updates

void map_view_set_fix(int32_t lat_e7, int32_t lon_e7, uint16_t speed_cms, uint16_t cog_cdeg)
{
    double nx = lon_to_nx(lon_e7 / 1e7), ny = lat_to_ny(lat_e7 / 1e7);
    uint32_t t = now_ms();
    double w = world_px();
    bool snap = !s_fix_live || !s_has_fix ||
                hypot((nx - s_mx) * w, (ny - s_my) * w) > SNAP_PX;
    if (snap) {
        s_mx = s_ax = s_bx = nx;
        s_my = s_ay = s_by = ny;
        s_anim = false;
    } else {
        // Glide from where the marker is now to the new fix over roughly the
        // fix interval: continuous motion instead of 1 Hz jumps.
        s_ax = s_mx;
        s_ay = s_my;
        s_bx = nx;
        s_by = ny;
        uint32_t dt = t - s_last_fix_ms;
        s_anim_dur = dt < 150 ? 150 : (dt > 1100 ? 1100 : dt);
        s_anim_t0 = t;
        s_anim = true;
    }
    s_has_fix = true;
    s_fix_live = true;
    s_last_fix_ms = t;
    s_speed_cms = speed_cms;
    if (speed_cms >= MOVING_ON_CMS) {
        s_moving = true;
    } else if (speed_cms < MOVING_OFF_CMS) {
        s_moving = false;
    }
    s_cog_cdeg = cog_cdeg;
    s_fix_lat = lat_e7;
    s_fix_lon = lon_e7;
    s_pos_dirty = true;
}

void map_view_set_dest(bool valid, int32_t lat_e7, int32_t lon_e7)
{
    if (!valid) {
        if (!s_has_dest) return;
        s_has_dest = false;
        invalidate();
        return;
    }
    double nx = lon_to_nx(lon_e7 / 1e7), ny = lat_to_ny(lat_e7 / 1e7);
    // Skip a repaint if it has not moved (sent only on change, but be sure).
    if (s_has_dest && fabs(nx - s_dnx) < 1e-9 && fabs(ny - s_dny) < 1e-9) return;
    s_dnx = nx;
    s_dny = ny;
    s_has_dest = true;
    s_line_phase = 0;   // start the march at the pattern origin
    invalidate();
}

void map_view_flush_state(void)
{
    if (s_pos_dirty && s_has_fix) {
        s_pos_dirty = false;
        settings_set_last_pos(s_fix_lat, s_fix_lon);
    }
    if (s_zoom_dirty) {
        s_zoom_dirty = false;
        settings_set_zoom((uint8_t)s_zoom);
    }
}

bool map_view_is_moving(void)
{
    return s_moving;
}

float map_view_rotation(void)
{
    return s_rot;
}

void map_view_set_link(bool connected, bool paired)
{
    s_connected = connected;
    s_paired = paired;
    update_status();
}

void map_view_tick(void)
{
    uint32_t t = now_ms();
    bool loaded = tiles_poll();

    if (s_anim) {
        float k = (float)(t - s_anim_t0) / (float)s_anim_dur;
        if (k >= 1.0f) {
            k = 1.0f;
            s_anim = false;
        }
        s_mx = s_ax + (s_bx - s_ax) * k;
        s_my = s_ay + (s_by - s_ay) * k;
    }
    if (s_follow && s_has_fix) {
        s_cx = s_mx;
        s_cy = s_my;
    }

    // Heading-up only while following a moving, live fix; otherwise ease
    // back to north-up (which also re-enables the fast unrotated blit).
    {
        bool heading_up = s_follow && s_has_fix && fix_is_live() && s_moving;
        float target = heading_up ? (float)s_cog_cdeg / 100.0f : 0.0f;
        float d = wrap180(target - s_rot);
        float dt = (t - s_rot_t) / 1000.0f;
        s_rot_t = t;
        if (dt > 0.1f) dt = 0.1f;
        if (fabsf(d) < 0.05f) {
            s_rot = heading_up ? wrap180(target) : 0.0f;
        } else {
            s_rot = wrap180(s_rot + d * (1.0f - expf(-dt * ROT_RATE)));
        }
    }

    bool nav_on = nav_view_active();
    if (nav_on != s_was_nav) {
        s_was_nav = nav_on;
        update_buttons();
    }
    // Cheap: update_status() early-returns unless the state or ETA minute
    // changed. Needed here so the ETA counts down while everything else is
    // steady.
    update_status();

    bool live = fix_is_live();
    if (live != s_was_live) {
        s_was_live = live;
        update_status();
        invalidate();  // marker colour
    }

    // Deferred NVS writes (GUI task is the only NVS writer).
    if (s_pos_dirty && (t - s_pos_saved_ms > SAVE_POS_MS || !s_pos_saved_ms)) {
        s_pos_dirty = false;
        s_pos_saved_ms = t;
        settings_set_last_pos(s_fix_lat, s_fix_lon);
    }
    if (s_zoom_dirty && t - s_zoom_changed_ms > SAVE_ZOOM_MS) {
        s_zoom_dirty = false;
        settings_set_zoom((uint8_t)s_zoom);
    }

    if (!s_map) return;

    // Both change the screen (or power off), so they run here rather than
    // inside the pill's event callback.
    if (s_sleep_req) {
        s_sleep_req = false;
        if (s_sleep_cb) s_sleep_cb();
        return;
    }
    if (s_reset_req) {
        s_reset_req = false;
        if (s_reset_cb) s_reset_cb();  // destroys this screen
        return;
    }

    if (loaded) {
        invalidate();
    } else {
        int32_t ox, oy, sx = 0, sy = 0;
        view_origin(&ox, &oy);
        if (s_has_fix) marker_screen(ox, oy, s_rot, &sx, &sy);
        // Marching dots on the destination line need a fresh frame ~10/s.
        bool line_anim = s_has_dest && s_has_fix;
        if (line_anim && t - s_line_anim_ms >= LINK_ANIM_MS) {
            s_line_anim_ms = t;
            lv_obj_invalidate(s_map);
        }
        if (!s_r_valid || ox != s_r_ox || oy != s_r_oy || sx != s_r_mx || sy != s_r_my ||
            lroundf(s_rot * 10.0f) != s_r_rot) {
            lv_obj_invalidate(s_map);
        }
    }

    if (t - s_prefetch_ms > PREFETCH_MS) {
        s_prefetch_ms = t;
        prefetch_ring();
    }
    if (s_toast_until && (int32_t)(t - s_toast_until) > 0) {
        s_toast_until = 0;
        lv_obj_set_hidden(s_toast, true);
    }
}
