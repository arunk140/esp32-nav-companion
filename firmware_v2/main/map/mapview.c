#include "map/mapview.h"
#include "map/tile_src.h"
#include "gfx/gfx.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "map";

// Background / gap colour. Also the screen background in the UI, so an
// uncovered edge never shows the panel's default (white).
#define MAP_BG GFX_RGB(0x1a, 0x1a, 0x1a)
#define DOT_FILL GFX_RGB(0x25, 0x63, 0xeb)
#define DOT_STALE GFX_RGB(0x6b, 0x72, 0x80)
#define DOT_RING_WHITE GFX_RGB(0xff, 0xff, 0xff)
#define MARK_R 9

// --- motion filter ---------------------------------------------------------
// The phone sends 1 Hz (and clumps ~10.5 s apart under Doze), so drawing a
// fix verbatim is a visible stutter: at z16 one 15 m step is 6.6 px. A
// time-constant ease makes motion continuous and wander sub-pixel. There is
// deliberately NO deadband: a deadband freezes the smoothed value while the
// vehicle keeps moving, so the target walks away and the map leaps when the
// gate reopens. That is a stutter generator, not a filter.
#define SMOOTH_TAU_S 0.35
// Sub-pixel residue is invisible; stop working once we get here, or the ease
// (and the redraw it schedules) runs for hundreds of frames after motion ends.
#define EASE_EPS_PX 0.05
// A fix this far away is a teleport (Doze batch, reconnect, a first fix far
// from the browse default). Snap rather than slide the map across screen.
#define SNAP_M 150.0
// Cap on-screen travel per iteration, so one huge jump cannot outrun a frame.
#define MAX_PAN_PX_S 480
#define MAX_DT_S 0.25

// Tile loading retry policy. A tile missing from the downloaded bbox must not
// spin forever, and a dropped request (full queue / dropped completion) must
// not wedge the cell.
#define LOAD_STALL_MS 3000
#define SLOT_TRIES 3
#define DRAG_APPLY_MS 33
#define ZOOM_COOL_MS 600

#define SCR 480

// --- state -----------------------------------------------------------------
static bool s_ready;
static int s_zoom = MAP_DEFAULT_ZOOM;

// The rectangle the map owns. The UI reserves a status bar above it.
static int s_vx, s_vy, s_vw, s_vh;

// Mosaic: a 3x3 grid of tiles at s_zoom with its top-left at (s_mtx, s_mty).
// s_mtx/s_mty always equal the derived grid origin, so the whole mosaic maps
// onto the screen with a single sub-tile offset.
static uint16_t *s_mosaic;
static int s_mtx, s_mty;
static int s_mosaic_zoom = -1;  // zoom the resident pixels belong to
static bool s_mosaic_init;

// Smoothed map centre and its target, in absolute fractional tile coords.
static double s_mcx, s_mcy;
static double s_tmcx, s_tmcy;
// Smoothed fix and its target.
static double s_mfx, s_mfy;
static double s_tfx, s_tfy;
static bool s_have_geom;

// Container offset: where the mosaic's top-left lands on screen.
static int s_px, s_py;

static double s_fix_lat, s_fix_lon;
static bool s_have_fix;
static double s_stored_lat, s_stored_lon;
static bool s_have_stored;
static int s_speed_cms;
static bool s_have_speed;
static double s_snap_t;  // SNAP_M in tile units at the current zoom

static bool s_follow = true;
static bool s_panned;         // a direct pan happened; force a redraw
static uint32_t s_zoom_cool;
static uint32_t s_gen;
static uint32_t s_tiles_loaded;

// Per-cell load state. A cell is "ok" when it holds exactly the tile the grid
// wants there, at the current zoom.
typedef enum { CELL_OK, CELL_LOAD, CELL_BUSY, CELL_GIVEUP } cell_t;
static cell_t s_cell[MAP_GRID * MAP_GRID];
static int s_cz[MAP_GRID * MAP_GRID], s_cx[MAP_GRID * MAP_GRID],
    s_cy[MAP_GRID * MAP_GRID];  // what the cell currently holds
static int s_wz[MAP_GRID * MAP_GRID], s_wx[MAP_GRID * MAP_GRID],
    s_wy[MAP_GRID * MAP_GRID];  // what the grid wants there
static uint32_t s_ct0[MAP_GRID * MAP_GRID];
static uint8_t s_ctry[MAP_GRID * MAP_GRID];
static int s_copen;  // cells not settled (O(1) idle test)

static void grid_align(void);

static void set_zoom_at(int z, double lat, double lon);
static void centre_snap(double lat, double lon);
static void cell_step(int i);

// --- projection ------------------------------------------------------------
static void gps_to_tile(double lat, double lon, double *tx, double *ty)
{
    double n = (double)(1 << s_zoom);
    double lat_rad = lat * M_PI / 180.0;
    *tx = (lon + 180.0) / 360.0 * n;
    *ty = (1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / M_PI) / 2.0 * n;
}

static void tile_to_gps(double tx, double ty, double *lat, double *lon)
{
    double n = (double)(1 << s_zoom);
    *lon = tx / n * 360.0 - 180.0;
    *lat = atan(sinh(M_PI * (1.0 - 2.0 * ty / n))) * 180.0 / M_PI;
}

static double mpp(double lat)
{
    return 156543.03392 * cos(lat * M_PI / 180.0) / (double)(1 << s_zoom);
}

static double tiles_per_m(double m, double lat)
{
    return m / mpp(lat);
}

static void snap_scale_update(double lat)
{
    s_snap_t = tiles_per_m(SNAP_M, lat);
}

// One frame-rate-independent exponential ease step. Returns true if moved.
static bool ease(double *x, double *y, double tx, double ty, double dt)
{
    double dx = tx - *x, dy = ty - *y;
    double d = sqrt(dx * dx + dy * dy);
    if (d * 256.0 < EASE_EPS_PX) {
        bool moved = (*x != tx || *y != ty);
        *x = tx;
        *y = ty;
        return moved;
    }
    double step = d * (1.0 - exp(-dt / SMOOTH_TAU_S));
    double cap = MAX_PAN_PX_S * dt / 256.0;
    if (step > cap) step = cap;
    *x += dx * (step / d);
    *y += dy * (step / d);
    return true;
}

// --- tile scheduling -------------------------------------------------------
// Does cell i hold exactly the tile the grid currently wants there?
//
// Deliberately NOT derived from s_cell[i]. That field is the loader
// scheduler's opinion of progress, and conflating "a load completed" with
// "the right pixels are in the mosaic" is how a cell gets marked CELL_OK
// while the mosaic is still gap colour.
static bool cell_holds_wanted(int i)
{
    return s_cz[i] == s_wz[i] && s_cx[i] == s_wx[i] && s_cy[i] == s_wy[i];
}

static void cell_set(int i, cell_t st)
{
    bool was = s_cell[i] != CELL_OK;
    bool is = st != CELL_OK;
    s_cell[i] = st;
    if (was && !is) s_copen--;
    else if (!was && is) s_copen++;
}

static void cell_request(void)
{
    s_gen++;
    s_mosaic_zoom = s_zoom;
    for (int r = 0; r < MAP_GRID; r++) {
        for (int c = 0; c < MAP_GRID; c++) {
            int i = r * MAP_GRID + c;
            s_wz[i] = s_zoom;
            s_wx[i] = s_mtx + c;
            s_wy[i] = s_mty + r;
            s_ctry[i] = 0;
            cell_set(i, CELL_LOAD);
        }
    }
    for (int i = 0; i < MAP_GRID * MAP_GRID; i++) cell_step(i);
}

static void cell_step(int i)
{
    if (s_cell[i] != CELL_LOAD) return;
    if (cell_holds_wanted(i)) {
        cell_set(i, CELL_OK);
        return;
    }
    if (s_ctry[i] >= SLOT_TRIES) {
        cell_set(i, CELL_GIVEUP);
        return;
    }
    tile_req_t req = {.slot = i, .z = s_wz[i], .x = s_wx[i], .y = s_wy[i],
                      .gen = s_gen};
    if (!tile_src_enqueue(&req)) return;  // queue full: retry next iteration
    s_ctry[i]++;
    s_ct0[i] = xTaskGetTickCount() * portTICK_PERIOD_MS;
    cell_set(i, CELL_BUSY);
}

// Slide the mosaic so the grid origin follows the centre. dx/dy are in whole
// tiles and are always within -(MAP_GRID-1)..(MAP_GRID-1): the centre moves by
// at most one tile per frame, and zoom changes skip the shift entirely.
//
// NOT called across a zoom change: after a zoom each cell already shows the
// right *area* at the previous scale (grid cell (r,c) wanted tile
// (mtx+c, mty+r) and still wants it), so leaving the pixels alone gives a free
// "sharpen" as the new tiles land. Shifting here would shuffle every tile to
// the wrong cell.
//
// Implementation note: this moves COLUMNS with a per-row memmove, then ROWS
// with one contiguous memmove. The column pass has to be per-row because the
// stride is MAP_MOSAIC_PX; getting that wrong is what smeared the whole
// mosaic with stray pixels.
static void mosaic_shift(int dx, int dy)
{
    if (!dx && !dy) return;
    const int W = MAP_MOSAIC_PX;
    if (dx > MAP_GRID - 1 || dx < -(MAP_GRID - 1)) dx = 0;
    if (dy > MAP_GRID - 1 || dy < -(MAP_GRID - 1)) dy = 0;
    if (!dx && !dy) return;

    if (dx > 0) {
        // Content moves left: old column (c+dx) becomes new column c.
        size_t n = (size_t)(MAP_GRID - dx) * 256 * sizeof(uint16_t);
        for (int y = 0; y < W; y++)
            memmove(&s_mosaic[(size_t)y * W], &s_mosaic[(size_t)y * W + dx * 256], n);
    } else if (dx < 0) {
        // Content moves right. Iterate upwards so the overlapping region is
        // never overwritten before it is read (memmove alone is not enough
        // when rows are independent but pixels within a row shift).
        size_t n = (size_t)(MAP_GRID + dx) * 256 * sizeof(uint16_t);
        for (int y = 0; y < W; y++)
            memmove(&s_mosaic[(size_t)y * W + (-dx) * 256], &s_mosaic[(size_t)y * W], n);
    }
    if (dy > 0) {
        size_t n = (size_t)(MAP_GRID - dy) * 256 * W * sizeof(uint16_t);
        memmove(s_mosaic, &s_mosaic[(size_t)dy * 256 * W], n);
    } else if (dy < 0) {
        size_t n = (size_t)(MAP_GRID + dy) * 256 * W * sizeof(uint16_t);
        memmove(&s_mosaic[(-dy) * 256 * W], s_mosaic, n);
    }
}

static void grid_align(void)
{
    int nx = (int)floor(s_mcx) - MAP_GRID / 2;
    int ny = (int)floor(s_mcy) - MAP_GRID / 2;
    if (s_mosaic_init && nx == s_mtx && ny == s_mty) return;
    int dx = s_mosaic_init ? nx - s_mtx : 0;
    int dy = s_mosaic_init ? ny - s_mty : 0;
    s_mosaic_init = true;
    if ((dx || dy) && s_mosaic_zoom == s_zoom) {
        mosaic_shift(dx, dy);
    } else if (dx || dy) {
        // Zoom changed: leave the pixels where they are (right area, wrong
        // scale) and let the reload refine them.
        ESP_LOGD(TAG, "zoom %d->%d: keeping old pixels in place", s_mosaic_zoom,
                 s_zoom);
    }
    s_mtx = nx;
    s_mty = ny;
    cell_request();
}

// --- rendering -------------------------------------------------------------
// Recompute the container offset so the map centre lands at the centre of the
// viewport. Because the grid origin is derived from the same centre, the
// offset can never drift out of range.
// Republish the container offset from the smoothed centre and the CURRENT grid
// origin.
//
// The offset is not free to float: it must stay inside one mosaic so the
// visible window never runs off the edge. With s_mcx - s_mtx == frac + 1 for
// frac in [0,1), the offset is in (-256, 0]; a value outside that means the
// grid origin and the centre disagree, which happens if grid_align() was
// skipped or ran with a stale centre. Clamp it and complain, rather than
// blitting from outside the mosaic (which is what turned the screen into gap
// colour).
static void place(void)
{
    grid_align();
    int px = s_vx + s_vw / 2 - (int)((s_mcx - s_mtx) * 256.0);
    int py = s_vy + s_vh / 2 - (int)((s_mcy - s_mty) * 256.0);
    if (px <= -MAP_TILE_PX || px > 0) {
        ESP_LOGE(TAG, "px %d out of range (mcx-mtx=%.3f)", px, s_mcx - s_mtx);
        px = 0;
    }
    if (py <= -MAP_TILE_PX || py > 0) {
        ESP_LOGE(TAG, "py %d out of range (mcy-mty=%.3f)", py, s_mcy - s_mty);
        py = 0;
    }
    s_px = px;
    s_py = py;
}

// The mosaic is MAP_MOSAIC_PX (768) square; the viewport is 480x450. The offset
// keeps the centre centred, so the visible slice of the mosaic moves by
// (-s_px, -s_py) and is always well inside the mosaic. assert-style guard so a
// future change cannot silently read outside it.
static int mosaic_src_x(int dx0) { return dx0 - s_px; }
static int mosaic_src_y(int dy0) { return dy0 - s_py; }

// Blit the visible slice of the mosaic into the viewport. The mosaic is only
// MAP_MOSAIC_PX square and the viewport is nearly as large, so there is always
// a sub-rect to copy; if the centre ever drifted outside that (it cannot,
// since the offset is derived from it) we paint the gap colour rather than
// leaving stale pixels on screen.
static void render_map(void)
{
    if (!s_mosaic) return;
    int dx0 = s_px, dy0 = s_py;
    int dx1 = s_px + MAP_MOSAIC_PX, dy1 = s_py + MAP_MOSAIC_PX;
    if (dx0 < s_vx) dx0 = s_vx;
    if (dy0 < s_vy) dy0 = s_vy;
    if (dx1 > s_vx + s_vw) dx1 = s_vx + s_vw;
    if (dy1 > s_vy + s_vh) dy1 = s_vy + s_vh;
    if (dx1 <= dx0 || dy1 <= dy0) {
        gfx_fill_rect(s_vx, s_vy, s_vw, s_vh, MAP_BG);
        return;
    }
    int sx = mosaic_src_x(dx0), sy = mosaic_src_y(dy0);
    if (sx < 0 || sy < 0 || sx + (dx1 - dx0) > MAP_MOSAIC_PX ||
        sy + (dy1 - dy0) > MAP_MOSAIC_PX) {
        // Can only happen if the mosaic is smaller than the viewport, which
        // the geometry prevents (768 > 480 and > 450). Paint the gap rather
        // than read out of bounds, and say so.
        ESP_LOGE(TAG, "mosaic slice OOB sx=%d sy=%d w=%d h=%d", sx, sy,
                 dx1 - dx0, dy1 - dy0);
        gfx_fill_rect(s_vx, s_vy, s_vw, s_vh, MAP_BG);
        return;
    }
    // Pass the mosaic's origin and the sub-rect position, rather than a
    // pre-offset pointer. gfx_blit_region adjusts the source itself when it
    // clips; pre-offsetting and then clipping skipped the source twice and
    // showed a completely different part of the mosaic.
    gfx_blit_region(dx0, dy0, dx1 - dx0, dy1 - dy0, s_mosaic, MAP_MOSAIC_PX,
                    sx, sy, MAP_MOSAIC_PX);
    gfx_dirty(dx0, dy0, dx1 - dx0, dy1 - dy0);
}

static void dot_screen(int *ox, int *oy)
{
    *ox = (int)((s_mfx - s_mtx) * 256.0) + s_px;
    *oy = (int)((s_mfy - s_mty) * 256.0) + s_py;
}

static void draw_dot(gfx_color_t fill)
{
    int x, y;
    dot_screen(&x, &y);
    if (x < -20 || y < -20 || x > SCR + 20 || y > SCR + 20) return;
    // White ring then fill, so the dot reads on any tile colour.
    gfx_circle(x, y, MARK_R + 2, DOT_RING_WHITE);
    gfx_circle(x, y, MARK_R, fill);
    gfx_dirty(x - MARK_R - 3, y - MARK_R - 3, 2 * (MARK_R + 3), 2 * (MARK_R + 3));
}

void mapview_dot_screen(int *x, int *y)
{
    dot_screen(x, y);
}

bool mapview_has_dot(void)
{
    return s_have_geom;
}


// --- public ----------------------------------------------------------------
void mapview_set_viewport(int x0, int y0, int w, int h)
{
    s_vx = x0;
    s_vy = y0;
    s_vw = w;
    s_vh = h;
}

bool mapview_init(void)
{
    if (s_ready) return true;
    if (s_vw <= 0 || s_vh <= 0) { s_vx = 0; s_vy = 0; s_vw = SCR; s_vh = SCR; }
    s_mosaic = heap_caps_malloc((size_t)MAP_MOSAIC_PX * MAP_MOSAIC_PX * 2,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_mosaic) {
        ESP_LOGE(TAG, "no PSRAM for the %dx%d mosaic", MAP_MOSAIC_PX,
                 MAP_MOSAIC_PX);
        return false;
    }
    // Paint the gap colour once so a cold mosaic is never garbage.
    for (int i = 0; i < MAP_MOSAIC_PX * MAP_MOSAIC_PX; i++) s_mosaic[i] = MAP_BG;

    s_zoom = settings_get_zoom();
    if (s_zoom < MAP_ZOOM_MIN) s_zoom = MAP_ZOOM_MIN;
    if (s_zoom > MAP_ZOOM_MAX) s_zoom = MAP_ZOOM_MAX;

    for (int i = 0; i < MAP_GRID * MAP_GRID; i++) {
        s_cz[i] = s_wz[i] = INT32_MIN;
        s_cell[i] = CELL_GIVEUP;
        s_ctry[i] = 0;
    }
    s_copen = 0;
    s_mosaic_init = false;
    s_mosaic_zoom = -1;
    s_follow = true;
    s_zoom_cool = 0;
    s_gen++;
    s_tiles_loaded = 0;

    tile_src_init();
    s_ready = true;

    // Boot view: the persisted fix if there is one, else the browse default.
    int32_t ll_lat = 0, ll_lon = 0;
    if (settings_get_last_pos(&ll_lat, &ll_lon)) {
        s_stored_lat = ll_lat / 1e7;
        s_stored_lon = ll_lon / 1e7;
        s_have_stored = true;
        centre_snap(s_stored_lat, s_stored_lon);
        mapview_set_position(s_stored_lat, s_stored_lon, false);
    } else {
        centre_snap(MAP_DEFAULT_LAT, MAP_DEFAULT_LON);
    }
    ESP_LOGI(TAG, "map up: z%d %s", s_zoom, s_have_stored ? "stored" : "default");
    return true;
}

void mapview_destroy(void)
{
    s_ready = false;
    s_have_fix = false;
    s_have_stored = false;
    s_have_geom = false;
    s_have_speed = false;
    s_follow = true;
    s_snap_t = 0.0;
    s_mosaic_init = false;
    s_mosaic_zoom = -1;
    s_copen = 0;
    for (int i = 0; i < MAP_GRID * MAP_GRID; i++) s_cell[i] = CELL_GIVEUP;
    s_gen++;  // in-flight loads are stale now
    // s_mosaic is intentionally kept: it is 1.1 MB of PSRAM and re-entering
    // the map reuses the pixels, so coming back is instant.
}

static void centre_snap(double lat, double lon)
{
    gps_to_tile(lat, lon, &s_tmcx, &s_tmcy);
    s_mcx = s_tmcx;
    s_mcy = s_tmcy;
    place();
    render_map();
    if (s_have_geom) draw_dot(s_have_fix ? DOT_FILL : DOT_STALE);
}

void mapview_set_position(double lat, double lon, bool live)
{
    if (!s_ready) return;
    if (live) {
        s_fix_lat = lat;
        s_fix_lon = lon;
        s_have_fix = true;
    }
    gps_to_tile(lat, lon, &s_tfx, &s_tfy);
    snap_scale_update(lat);
    if (!s_have_geom) {
        s_mfx = s_tfx;
        s_mfy = s_tfy;
        s_have_geom = true;
    } else {
        double dx = s_tfx - s_mfx, dy = s_tfy - s_mfy;
        if (dx * dx + dy * dy > s_snap_t * s_snap_t) {
            s_mfx = s_tfx;
            s_mfy = s_tfy;
        }
    }
    if (!live) {
        s_mfx = s_tfx;
        s_mfy = s_tfy;
    }
    if (s_have_geom) draw_dot(live ? DOT_FILL : DOT_STALE);
}

// Re-derive the dot's tile coords after a zoom. They are absolute tile units
// and the zoom changes what those units mean, so a zoom MUST reproject them:
// carrying the old numbers put the dot about 1.2e7 px off the grid.
static void fix_reproject(void)
{
    double lat, lon;
    if (s_have_fix) { lat = s_fix_lat; lon = s_fix_lon; }
    else if (s_have_stored) { lat = s_stored_lat; lon = s_stored_lon; }
    else return;
    gps_to_tile(lat, lon, &s_tfx, &s_tfy);
    s_mfx = s_tfx;
    s_mfy = s_tfy;
    s_have_geom = true;
}

// Zoom anchored so that (lat,lon) keeps its screen position.
//
// Tile coordinates scale exactly by k = 2^dz, and a tile is 256 px at every
// zoom (the geographic span per tile changes, the pixel size does not), so
// preserving the anchor's screen offset means preserving its offset from the
// map centre in tile units:
//     (ox*k - new_centre) * 256 = (ox - centre) * 256
//  => new_centre = centre + ox*(k - 1)
// With the anchor at the map centre this degenerates to ox*k, which is why
// zooming at screen centre holds the view exactly.
static void set_zoom_at(int z, double lat, double lon)
{
    if (z < MAP_ZOOM_MIN) z = MAP_ZOOM_MIN;
    if (z > MAP_ZOOM_MAX) z = MAP_ZOOM_MAX;
    if (z == s_zoom) return;

    double ox, oy;
    gps_to_tile(lat, lon, &ox, &oy);
    double k = ldexp(1.0, z - s_zoom);

    if (s_follow) s_follow = false;  // manual zoom breaks follow

    s_zoom = z;
    settings_set_zoom(z);
    s_tmcx = s_mcx += ox * (k - 1.0);
    s_tmcy = s_mcy += oy * (k - 1.0);
    fix_reproject();
    snap_scale_update(s_have_fix ? s_fix_lat : s_stored_lat);
    place();
    render_map();
    if (s_have_geom) draw_dot(s_have_fix ? DOT_FILL : DOT_STALE);
    ESP_LOGI(TAG, "zoom %d", z);
}

void mapview_set_zoom(int z)
{
    if (!s_ready) return;
    // Anchor on the map centre, which is the screen centre by construction.
    double lat, lon;
    tile_to_gps(s_mcx, s_mcy, &lat, &lon);
    set_zoom_at(z, lat, lon);
}

int mapview_get_zoom(void)
{
    return s_zoom;
}

void mapview_zoom_step(int delta)
{
    if (!s_ready) return;
    // The buttons anchor on the screen centre, which by construction is the
    // map centre -- so the anchor geo is just the centre's own geo.
    double lat, lon;
    tile_to_gps(s_mcx, s_mcy, &lat, &lon);
    set_zoom_at(s_zoom + delta, lat, lon);
}

// Pinch: keep the geo under (mid_x, mid_y) fixed on screen.
void mapview_pinch(int delta, int mid_x, int mid_y)
{
    if (!s_ready || delta == 0) return;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if ((int)(now - s_zoom_cool) < ZOOM_COOL_MS) return;
    s_zoom_cool = now;

    double fx = s_mtx + (mid_x - s_px) / 256.0;
    double fy = s_mty + (mid_y - s_py) / 256.0;
    double lat, lon;
    tile_to_gps(fx, fy, &lat, &lon);
    set_zoom_at(s_zoom + delta, lat, lon);
}

void mapview_recenter(void)
{
    if (!s_ready) return;
    s_follow = true;
    if (s_have_fix) centre_snap(s_fix_lat, s_fix_lon);
    else if (s_have_stored) centre_snap(s_stored_lat, s_stored_lon);
    else centre_snap(MAP_DEFAULT_LAT, MAP_DEFAULT_LON);
}

// Pan by a screen-space delta (finger moved right => map moves right).
//
// A flag rather than an immediate redraw: mapview_update() owns the render
// path, and after a direct pan the ease has no residual, so it would report
// "nothing moved" and the frame would never be drawn.
void mapview_pan(int dx, int dy)
{
    if (!s_ready) return;
    if (s_follow) s_follow = false;
    // Cap per call: a slow frame lets the gesture deliver a big delta, and
    // applying it whole causes a multi-tile jump -> a full mosaic slide and a
    // fresh 3-tile reload -> another slow frame. Drop the excess; the map
    // cannot outrun the card.
    if (dx > 256) dx = 256; else if (dx < -256) dx = -256;
    if (dy > 256) dy = 256; else if (dy < -256) dy = -256;
    double mx = (double)dx / 256.0, my = (double)dy / 256.0;
    // Finger right => map right => centre left.
    s_mcx -= mx; s_tmcx -= mx;
    s_mcy -= my; s_tmcy -= my;
    s_panned = true;
}

void mapview_set_speed(int cms)
{
    s_speed_cms = cms;
    s_have_speed = cms >= 0;
}

void mapview_update(uint32_t now_ms)
{
    static uint32_t last;
    if (!s_ready) return;
    double dt = (int)(now_ms - last) / 1000.0;
    last = now_ms;
    if (dt < 0.001) dt = 0.001; else if (dt > MAX_DT_S) dt = MAX_DT_S;

    // Follow: the centre chases the smoothed fix, so the map and the dot are
    // the same eased value and cannot disagree.
    bool moved = false;
    if (s_follow && s_have_geom) {
        s_tmcx = s_tfx;
        s_tmcy = s_tfy;
        double dx = s_tmcx - s_mcx, dy = s_tmcy - s_mcy;
        if (dx * dx + dy * dy > s_snap_t * s_snap_t) {
            s_mcx = s_tmcx;
            s_mcy = s_tmcy;
            moved = true;
        }
    }
    if (ease(&s_mcx, &s_mcy, s_tmcx, s_tmcy, dt)) moved = true;
    if (s_have_geom && ease(&s_mfx, &s_mfy, s_tfx, s_tfy, dt)) moved = true;
    if (s_panned) { s_panned = false; moved = true; }

    // An idle map must cost almost nothing: no blit, no invalidate.
    if (moved) {
        place();
        render_map();
        if (s_have_geom) draw_dot(s_have_fix ? DOT_FILL : DOT_STALE);
    }

    // Drain finished loads and copy them into the mosaic.
    if (tile_src_active()) {
        tile_done_t d;
        while (tile_src_poll(&d)) {
            if (d.gen != s_gen || d.slot < 0 || d.slot >= MAP_GRID * MAP_GRID) {
                if (d.scratch) tile_src_release(d.scratch);
                continue;
            }
            if (d.scratch) {
                int r = d.slot / MAP_GRID, c = d.slot % MAP_GRID;
                // The cell still wants this tile, and no zoom change
                // intervened: commit into its mosaic rectangle.
                if (d.z == s_wz[d.slot] && d.x == s_wx[d.slot] &&
                    d.y == s_wy[d.slot] && s_mosaic_zoom == s_zoom) {
                    // Write the cell ROW BY ROW, not as one 128 KiB block.
                    // The mosaic is 768 px wide with a 768 px stride, so a
                    // cell is 256 rows of 256 px that are NOT contiguous: a
                    // single memcpy spills each row into the next cell and
                    // walks off the end of the mosaic, which is how the map
                    // ended up mostly gap colour with a scrambled band while
                    // every cell reported itself as loaded.
                    uint16_t *dst = &s_mosaic[((size_t)r * MAP_TILE_PX) * MAP_MOSAIC_PX
                                              + c * MAP_TILE_PX];
                    for (int row = 0; row < MAP_TILE_PX; row++)
                        memcpy(&dst[(size_t)row * MAP_MOSAIC_PX],
                               &d.scratch[(size_t)row * MAP_TILE_PX * 2],
                               MAP_TILE_PX * 2);
                    s_cz[d.slot] = d.z;
                    s_cx[d.slot] = d.x;
                    s_cy[d.slot] = d.y;
                    s_tiles_loaded++;
                    // One-shot: the first time a whole grid is resident we
                    // know the card is readable and the mosaic is coherent.
                    if (s_tiles_loaded == MAP_GRID * MAP_GRID)
                        ESP_LOGI(TAG, "first grid complete at z%d (%d,%d)",
                                 s_zoom, s_mtx, s_mty);
                    // Re-render the viewport. Marking a cell dirty is NOT
                    // enough: the framebuffer still holds the old mosaic
                    // content, so a dirty mark alone re-pushes stale pixels
                    // and the map stays as it was before the load. A per-cell
                    // blit is the obvious optimisation and it is WRONG twice
                    // over -- the mosaic has a 768 px stride so one cell's
                    // source rows are not contiguous, and the gap-colour
                    // regions around it stay inconsistent. 480x450 is a
                    // 432 KB memcpy, noise next to a 128 KB SD read.
                    (void)r;
                    (void)c;
                    render_map();
                }
                tile_src_release(d.scratch);
            }
            cell_set(d.slot, CELL_LOAD);
            cell_step(d.slot);
        }
    }

    // The completion path is lossy by design; re-arm anything that has been
    // waiting implausibly long. 9 sequential reads is ~1.2 s worst case.
    if (s_copen) {
        for (int i = 0; i < MAP_GRID * MAP_GRID; i++) {
            if (s_cell[i] == CELL_BUSY &&
                (int)(now_ms - s_ct0[i]) >= LOAD_STALL_MS)
                cell_set(i, CELL_LOAD);
            cell_step(i);
        }
    }
}

const map_status_t *mapview_status(void)
{
    static map_status_t st;
    st.have_fix = s_have_fix;
    st.have_stored = s_have_stored;
    st.lat = s_fix_lat;
    st.lon = s_fix_lon;
    st.have_speed = s_have_speed;
    st.speed_cms = s_speed_cms;
    return &st;
}

uint32_t mapview_tiles_loaded(void)
{
    return s_tiles_loaded;
}
