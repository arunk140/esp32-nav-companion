#include "map/map_view.h"
#include "map/loader.h"
#include "map_tiles.h"
#include "display/lvgl9_port.h"  // touch_last_sample (sole touch reader is LVGL)
#include "settings.h"            // last known position (gray marker)
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "map";

// Browse default: Gateway of India. z16 -> ~0.58 km/tile,
// 3x3 grid = 768px = 1.7 km across, 480px screen shows ~1.1 km.
#define DEFAULT_LAT 18.9219
#define DEFAULT_LON 72.8347
#define DEFAULT_ZOOM 16
#define ZOOM_MIN 15
#define ZOOM_MAX 17

#define GRID 3
#define TILE 256
#define SCR 480
#define CENTER_PX ((GRID * TILE - SCR) / 2)  // 144: grid centre == screen centre

static map_tiles_handle_t s_map;
static lv_obj_t *s_box;              // 768x768 tile container
static lv_obj_t *s_imgs[GRID * GRID];
static int s_gx, s_gy;               // origin tile (top-left)
static int s_px, s_py;               // container position (negative offsets)
static int s_zoom = DEFAULT_ZOOM;
static double s_clat = DEFAULT_LAT, s_clon = DEFAULT_LON;  // grid-centre geo
static bool s_ready;  // set at end of map_view_create; gates poll/apply
static double s_last_lat, s_last_lon;  // last live fix (CMD/POSITION path)
static bool s_have_fix;                // a live position has arrived
static double s_stored_lat, s_stored_lon;  // last persisted fix (gray marker)
static bool s_have_stored;
// Marker animation: target (fix) vs rendered (eased) in fractional tiles.
static double s_mtx, s_mty, s_mrx, s_mry;
static double s_atx, s_aty;  // last applied target (fractional tiles)
static bool s_have_target;
// Follow mode: live fixes recenter the map (deadband: only outside inner
// tile). Any manual drag/zoom clears it; Center re-engages it. Starts ON so
// a fresh connect follows until first touched.
static bool s_follow = true;
// Jitter gate: ignore live fixes that move the target less than this many
// meters (phone GPS wanders; the easing animation would chase noise).
#define JITTER_M 15.0
static void map_place_marker(void);
// Drag accumulator: touch events (~200 Hz) only add deltas here.
// map_view_apply() consumes them at 30 Hz so SD reloads never run in an
// event handler and the map pans smoothly between tile shifts.
static int s_acc_x, s_acc_y;

void map_accum(int dx, int dy)
{
    s_acc_x += dx;
    s_acc_y += dy;
}

// Grid-frame shift: gx/gy just moved by (dx, dy) tiles, so every tile-space
// coordinate (animation target/rendered, jitter reference) must move the
// opposite way or the marker teleports. Call on every origin change.
static void frame_shift(int dx, int dy)
{
    s_mtx -= dx;
    s_mty -= dy;
    s_mrx -= dx;
    s_mry -= dy;
    s_atx -= dx;
    s_aty -= dy;
}

static void map_update_center_btn(void);  // defined below (needs buttons)
static void grid_request(void);  // defined below (needs grid state)

// Consume accumulated drag: move container, shift grid by whole tiles,
// reload changed tiles. Runs at most every 33 ms from map_view_poll().
static void map_view_apply(void)
{
    if (!s_acc_x && !s_acc_y) return;
    int dx = s_acc_x, dy = s_acc_y;
    s_acc_x = s_acc_y = 0;
    // Manual drag breaks follow mode (re-lock via Center).
    if (s_follow) {
        s_follow = false;
        map_update_center_btn();
    }
    // Cap per apply: during a blocking reload the accumulator keeps growing,
    // and applying it all at once causes a multi-tile jump -> full reload ->
    // another block (death spiral). Drop the excess; the map can't outrun SD.
    if (dx > TILE) dx = TILE; else if (dx < -TILE) dx = -TILE;
    if (dy > TILE) dy = TILE; else if (dy < -TILE) dy = -TILE;
    s_px += dx;
    s_py += dy;
    // Whole-tile shifts, with slot rotation so unchanged tiles stay resident:
    // rotate FIRST (buffers follow their tile), then reload (only the wrapped
    // edge misses and hits SD). Without rotation every shift costs 9 loads.
    int sdx = 0, sdy = 0;
    while (s_px <= -TILE) { s_gx++; s_px += TILE; sdx++; }
    while (s_px > 0)      { s_gx--; s_px -= TILE; sdx--; }
    while (s_py <= -TILE) { s_gy++; s_py += TILE; sdy++; }
    while (s_py > 0)      { s_gy--; s_py -= TILE; sdy--; }
    lv_obj_set_pos(s_box, s_px, s_py);
    bool shifted = (sdx || sdy);
    if (shifted) {
        map_tiles_shift_slots(s_map, sdx, sdy);
        // Marker tile-coords live in the grid frame: shift them too, or the
        // dot teleports on every drag-shift (then snaps back on next fix).
        frame_shift(sdx, sdy);
        // New grid-centre geo for later zoom ops.
        map_tiles_tile_xy_to_gps(s_map, s_gx + GRID / 2.0, s_gy + GRID / 2.0,
                                 &s_clat, &s_clon);
        grid_request();
    }
}

static uint32_t s_gen;       // invalidates in-flight loads on a new batch
static int s_pending;        // unresolved loads of the current generation
static int s_scale;          // zoom preview scale (0 = none, else LVGL units)
static bool s_qpend[GRID * GRID];  // per-slot duplicate guard within a batch

// Enqueue every non-resident slot of the current grid; link resident slots
// immediately. Returns instantly — the loader task does the SD reads and
// grid_drain() (below, from the GUI task) swaps buffers in.
static void grid_request(void)
{
    s_gen++;
    s_pending = 0;
    memset(s_qpend, 0, sizeof(s_qpend));
    for (int row = 0; row < GRID; row++) {
        for (int col = 0; col < GRID; col++) {
            int i = row * GRID + col;
            int tx = s_gx + col, ty = s_gy + row;
            if (map_tiles_slot_loaded(s_map, i, tx, ty)) {
                lv_image_set_src(s_imgs[i], map_tiles_get_image(s_map, i));
                continue;
            }
            loader_enqueue(i, s_zoom, tx, ty, s_gen);
            s_pending++;
            s_qpend[i] = true;
        }
    }
}

// Drain loader completions: memcpy scratch into the slot buffer (frame-atomic:
// only the GUI task touches LVGL + slot buffers), re-link, release scratch.
// Drops stale generations and clears the zoom preview when current lands.
static void grid_drain(void)
{
    load_done_t d;
    while (loader_poll_done(&d)) {
        if (d.gen != s_gen) {
            if (d.scratch) loader_release(d.scratch);
            continue;
        }
        if (d.ok && d.scratch) {
            uint8_t *buf = map_tiles_slot_ensure(s_map, d.slot);
            if (buf) {
                memcpy(buf, d.scratch, LOADER_TILE_BYTES);
                map_tiles_slot_commit(s_map, d.slot, d.x, d.y);
                lv_image_set_src(s_imgs[d.slot],
                                 map_tiles_get_image(s_map, d.slot));
                lv_obj_invalidate(s_imgs[d.slot]);
            }
            loader_release(d.scratch);
        } else {
            lv_image_set_src(s_imgs[d.slot], NULL);  // gap shows box bg
            lv_obj_invalidate(s_imgs[d.slot]);
        }
        s_qpend[d.slot] = false;
        if (s_pending > 0) s_pending--;
        if (s_pending == 0 && s_scale) {
            s_scale = 0;
            lv_obj_set_style_transform_scale_x(s_box, 256, 0);
            lv_obj_set_style_transform_scale_y(s_box, 256, 0);
        }
    }
}

// Recenters grid so (lat,lon) sits at grid centre. Requests tiles async.
static void recenter(double lat, double lon)
{
    s_clat = lat;
    s_clon = lon;
    map_tiles_set_center_from_gps(s_map, lat, lon);
    map_tiles_get_position(s_map, &s_gx, &s_gy);
    s_px = -CENTER_PX;
    s_py = -CENTER_PX;
    lv_obj_set_pos(s_box, s_px, s_py);
    grid_request();
}

// Follow-mode centering: like recenter(), but positions the grid with
// sub-tile precision so the fix lands on the EXACT screen centre instead of
// up to 128 px off (the deadband offset). Same-origin fixes cost no SD reads
// (slots already resident); only the container moves.
static void recenter_exact(double lat, double lon)
{
    s_clat = lat;
    s_clon = lon;
    double x, y;
    map_tiles_gps_to_tile_xy(s_map, lat, lon, &x, &y);
    int tx = (int)x - GRID / 2;
    int ty = (int)y - GRID / 2;
    double fx = x - (int)x, fy = y - (int)y;
    if (tx == s_gx && ty == s_gy) {
        s_px = -CENTER_PX - (fx - 0.5) * TILE;
        s_py = -CENTER_PX - (fy - 0.5) * TILE;
        lv_obj_set_pos(s_box, s_px, s_py);
        return;
    }
    map_tiles_set_center_from_gps(s_map, lat, lon);
    map_tiles_get_position(s_map, &s_gx, &s_gy);
    s_px = -CENTER_PX - (fx - 0.5) * TILE;
    s_py = -CENTER_PX - (fy - 0.5) * TILE;
    lv_obj_set_pos(s_box, s_px, s_py);
    grid_request();
}

static lv_obj_t *s_btn_in, *s_btn_out;
static lv_obj_t *s_btn_center;
static void map_update_zoom_btns(void);

// Center button is only useful when NOT following: hidden while centered,
// reappears on any manual drag/zoom.
static void map_update_center_btn(void)
{
    if (!s_btn_center) return;
    if (s_follow) lv_obj_add_flag(s_btn_center, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_btn_center, LV_OBJ_FLAG_HIDDEN);
}

// Geo under a screen point (for zoom anchors). Inverts the container offset:
// screen (sx,sy) -> box (sx-px, sy-py) -> fractional tile -> GPS.
static void screen_to_geo(int sx, int sy, double *lat, double *lon)
{
    double fx = s_gx + (sx - s_px) / (double)TILE;
    double fy = s_gy + (sy - s_py) / (double)TILE;
    map_tiles_tile_xy_to_gps(s_map, fx, fy, lat, lon);
}

static void set_zoom(int z)
{
    if (z < ZOOM_MIN) z = ZOOM_MIN;
    if (z > ZOOM_MAX) z = ZOOM_MAX;
    if (z == s_zoom) return;
    // Manual zoom breaks follow mode (re-lock via Center).
    if (s_follow) {
        s_follow = false;
        map_update_center_btn();
    }
    s_zoom = z;
    settings_set_zoom(z);  // persisted; zoom changes are rare (GUI task: safe)
    map_tiles_set_zoom(s_map, z);
    recenter(s_clat, s_clon);
    map_update_zoom_btns();
    ESP_LOGI(TAG, "zoom %d", z);
}

// Zoom anchored on a fixed geo point (stays put on screen across the zoom).
static void set_zoom_at(int z, double lat, double lon)
{
    if (z < ZOOM_MIN) z = ZOOM_MIN;
    if (z > ZOOM_MAX) z = ZOOM_MAX;
    if (z == s_zoom) {
        // Already there: still re-lock semantics unchanged, just recenter.
        recenter(lat, lon);
        return;
    }
    if (s_follow) {
        s_follow = false;
        map_update_center_btn();
    }
    bool zoom_in = z > s_zoom;
    s_zoom = z;
    settings_set_zoom(z);
    map_tiles_set_zoom(s_map, z);
    recenter(lat, lon);
    // Instant preview: scale old content around the anchor (pivot in box
    // coords) while the new zoom loads async; grid_drain() unscales once the
    // generation lands. In: 200%, out: 50%.
    {
        double fx, fy;
        map_tiles_gps_to_tile_xy(s_map, lat, lon, &fx, &fy);
        int pivx = (int)((fx - s_gx) * TILE);
        int pivy = (int)((fy - s_gy) * TILE);
        s_scale = zoom_in ? 512 : 128;
        lv_obj_set_style_transform_pivot_x(s_box, pivx, 0);
        lv_obj_set_style_transform_pivot_y(s_box, pivy, 0);
        lv_obj_set_style_transform_scale_x(s_box, s_scale, 0);
        lv_obj_set_style_transform_scale_y(s_box, s_scale, 0);
    }
    map_update_zoom_btns();
}

// Drag: LVGL pointer vect applied to container; normalize by whole tiles.
// Drag: track pointer positions manually. lv_indev_get_vect() only accumulates
// for scrollable objects, so it reads 0,0 here — do the delta ourselves from
// PRESSED/PRESSING/RELEASED (all bubble up from the tile images).
static void drag_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    static lv_point_t last = {0, 0};
    static bool down = false;
    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(indev, &last);
        down = true;
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        down = false;
        return;
    }
    if (code != LV_EVENT_PRESSING || !down) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int dx = p.x - last.x, dy = p.y - last.y;
    last = p;
    if (dx || dy) {
        // Accumulate only — map_view_poll() applies at 30 Hz (see above).
        map_accum(dx, dy);
    }
    return;
}

static lv_obj_t *s_btn_in, *s_btn_out;

// Gray out zoom buttons at the limits (ASCII-safe, no icon fonts needed).
// (Prototype is up with set_zoom; definition here.)
static void map_update_zoom_btns(void)
{
    if (s_btn_in) {
        if (s_zoom >= ZOOM_MAX) lv_obj_add_state(s_btn_in, LV_STATE_DISABLED);
        else lv_obj_remove_state(s_btn_in, LV_STATE_DISABLED);
    }
    if (s_btn_out) {
        if (s_zoom <= ZOOM_MIN) lv_obj_add_state(s_btn_out, LV_STATE_DISABLED);
        else lv_obj_remove_state(s_btn_out, LV_STATE_DISABLED);
    }
}

static void btn_zoom_in(lv_event_t *e)
{
    (void)e;
    double lat, lon;
    screen_to_geo(SCR / 2, SCR / 2, &lat, &lon);  // anchor: screen centre
    set_zoom_at(s_zoom + 1, lat, lon);
}
static void btn_zoom_out(lv_event_t *e)
{
    (void)e;
    double lat, lon;
    screen_to_geo(SCR / 2, SCR / 2, &lat, &lon);  // anchor: screen centre
    set_zoom_at(s_zoom - 1, lat, lon);
}
static void recenter_preferred(void)
{
    // Live fix > stored fix > browse default. Re-locks follow mode.
    s_follow = true;
    map_update_center_btn();
    if (s_have_fix) recenter(s_last_lat, s_last_lon);
    else if (s_have_stored) recenter(s_stored_lat, s_stored_lon);
    else recenter(DEFAULT_LAT, DEFAULT_LON);
}

static void btn_recenter(lv_event_t *e)
{
    (void)e;
    recenter_preferred();
}

void map_view_set_zoom(int z)
{
    set_zoom(z);
}

void map_view_recenter_default(void)
{
    // CMD 0x01: live fix > stored fix > browse default.
    recenter_preferred();
}

static lv_obj_t *s_marker;  // position dot, created on first fix

void map_view_set_position(double lat, double lon, bool live)
{
    if (!s_ready) return;
    if (live) {
        s_last_lat = lat;
        s_last_lon = lon;
        s_have_fix = true;
    }
    double fx, fy;
    map_tiles_gps_to_tile_xy(s_map, lat, lon, &fx, &fy);
    if (live && s_have_target) {
        // Jitter gate: sub-threshold wander never becomes a target.
        double mpp = 156543.03392 * cos(lat * M_PI / 180.0) / (1 << s_zoom);
        double dxm = (fx - s_atx) * TILE * mpp;
        double dym = (fy - s_aty) * TILE * mpp;
        if (dxm * dxm + dym * dym < JITTER_M * JITTER_M) {
            return;
        }
    }
    // Outside the inner tile? Recenter so the fix sits at grid centre —
    // but ONLY in follow mode. A manual browse never gets yanked back.
    // Follow uses exact centering (dot on screen centre); browse keeps the
    // deadband recenter only via the Center button.
    if (live && s_follow) {
        recenter_exact(lat, lon);
        map_tiles_gps_to_tile_xy(s_map, lat, lon, &fx, &fy);
        // Map and dot move together: snap the animation to the new centre.
        s_mrx = fx;
        s_mry = fy;
    } else if (fx < s_gx + 1 || fx >= s_gx + 2 || fy < s_gy + 1 || fy >= s_gy + 2) {
        // Not following and fix off-grid (e.g. boot with stored fix far from
        // the default view): center it once so the marker is visible.
        if (!s_have_target) {
            recenter(lat, lon);
            map_tiles_gps_to_tile_xy(s_map, lat, lon, &fx, &fy);
            s_mrx = fx;
            s_mry = fy;
        }
        // Otherwise leave the grid alone; the marker may sit off-screen.
    }
    s_mtx = fx;
    s_mty = fy;
    s_atx = fx;
    s_aty = fy;
    s_have_target = true;
    if (!s_marker) {
        s_marker = lv_obj_create(s_box);
        lv_obj_set_size(s_marker, 20, 20);
        lv_obj_set_style_radius(s_marker, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_marker, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s_marker, lv_color_white(), 0);
        lv_obj_set_style_border_width(s_marker, 3, 0);
        lv_obj_remove_flag(s_marker, LV_OBJ_FLAG_SCROLLABLE);
        // First fix: appear already there.
        s_mrx = fx;
        s_mry = fy;
    }
    // Blue = live fix, gray = stored fix with no phone.
    lv_obj_set_style_bg_color(s_marker, lv_color_hex(live ? 0x2563eb : 0x6b7280), 0);
    map_place_marker();
}

// Ease the rendered marker toward the target each poll (~200 Hz, k=0.02 ->
// ~98% in 1 s). Fixes arrive at 1 Hz (or batched), so this is what makes
// motion look continuous instead of steppy.
static void map_animate_marker(void)
{
    if (!s_marker) return;
    double dx = s_mtx - s_mrx, dy = s_mty - s_mry;
    if (dx * dx + dy * dy < 0.002 * 0.002) {
        if (s_mrx != s_mtx || s_mry != s_mty) {
            s_mrx = s_mtx;
            s_mry = s_mty;
            map_place_marker();
        }
        return;
    }
    s_mrx += dx * 0.02;
    s_mry += dy * 0.02;
    map_place_marker();
}

static void map_place_marker(void)
{
    lv_obj_set_pos(s_marker, (int)((s_mrx - s_gx) * TILE) - 10,
                              (int)((s_mry - s_gy) * TILE) - 10);
}

static lv_obj_t *mk_btn(lv_obj_t *parent, const char *txt, int x, int y)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 52, 52);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_center(l);
    return b;
}

void map_view_create(lv_obj_t *parent)
{
    // The screen itself must not scroll — only the tile box pans. Otherwise
    // buttons and status ride along with the drag.
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_OFF);

    const char *folders[] = {"street_map"};
    // Restore persisted zoom (button states + tile set follow it).
    s_zoom = settings_get_zoom();
    map_tiles_config_t cfg = {
        .base_path = "/sdcard",
        .tile_folders = {folders[0]},
        .tile_type_count = 1,
        .default_zoom = s_zoom,
        .use_spiram = true,
        .default_tile_type = 0,
        .grid_cols = GRID,
        .grid_rows = GRID,
    };
    // Handle is an app singleton (loader caches it): init once, never freed.
    // Re-entering with a live handle just reuses buffers (instant map).
    if (!s_map) {
        s_map = map_tiles_init(&cfg);
        if (!s_map) {
            lv_obj_t *l = lv_label_create(parent);
            lv_label_set_text(l, "No map: SD missing?");
            lv_obj_center(l);
            ESP_LOGE(TAG, "map_tiles_init failed");
            return;
        }
    }
    // Loader task + queues persist across show_map cycles; start once.
    // (Holds s_map for file reads; recreated handle is assigned to s_map
    // before any enqueue, and loader only dereferences it per-request.)
    static bool loader_started;
    if (!loader_started) {
        loader_started = true;
        loader_init(s_map);
    }

    s_box = lv_obj_create(parent);
    lv_obj_set_size(s_box, GRID * TILE, GRID * TILE);
    lv_obj_set_style_pad_all(s_box, 0, 0);
    lv_obj_set_style_border_width(s_box, 0, 0);
    lv_obj_set_style_radius(s_box, 0, 0);
    lv_obj_set_style_bg_color(s_box, lv_color_hex(0x1a1a1a), 0);  // gaps
    lv_obj_add_event_cb(s_box, drag_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_box, drag_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_box, drag_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_box, drag_cb, LV_EVENT_PRESS_LOST, NULL);

    for (int row = 0; row < GRID; row++) {
        for (int col = 0; col < GRID; col++) {
            int i = row * GRID + col;
            s_imgs[i] = lv_image_create(s_box);
            lv_obj_set_pos(s_imgs[i], col * TILE, row * TILE);
        }
    }

    // Zoom + recenter: right edge / bottom centre, inside safe circle.
    s_btn_in = mk_btn(parent, "+", 404, 176);
    lv_obj_add_event_cb(s_btn_in, btn_zoom_in, LV_EVENT_CLICKED, NULL);
    s_btn_out = mk_btn(parent, "-", 404, 240);
    lv_obj_add_event_cb(s_btn_out, btn_zoom_out, LV_EVENT_CLICKED, NULL);
    lv_obj_t *br = lv_button_create(parent);
    s_btn_center = br;
    lv_obj_set_size(br, 64, 44);
    lv_obj_set_pos(br, 208, 412);
    lv_obj_t *rl = lv_label_create(br);
    lv_label_set_text(rl, "C");  // recenter; ASCII only (LVGL font has no symbols)
    lv_obj_center(rl);
    lv_obj_add_event_cb(br, btn_recenter, LV_EVENT_CLICKED, NULL);

    recenter(DEFAULT_LAT, DEFAULT_LON);
    map_update_zoom_btns();
    map_update_center_btn();  // hidden initially (following)
    // Boot to the last known fix if there is one (gray = stale, no phone).
    int32_t ll_lat = 0, ll_lon = 0;
    if (settings_get_last_pos(&ll_lat, &ll_lon)) {
        double la = ll_lat / 1e7, lo = ll_lon / 1e7;
        s_stored_lat = la;
        s_stored_lon = lo;
        s_have_stored = true;
        recenter(la, lo);
        map_view_set_position(la, lo, false);
    }
    s_ready = true;
    ESP_LOGI(TAG, "browse map ready (no location)");
}

// Pinch: consume the shared touch sample (see lvgl9_port). LVGL's indev is the
// sole I2C reader; reading the controller here would invalidate LVGL's sample.
// >50px manhattan change + 600 ms cooldown.
void map_view_poll(void)
{
    if (!s_ready) return;
    map_animate_marker();
    grid_drain();  // async tile completions (memcpy + re-link, GUI task)
    // 30 Hz drag application (see accumulator above).
    static uint32_t last_apply;
    uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if ((int)(now_ms - last_apply) > 33) {
        last_apply = now_ms;
        map_view_apply();
    }
    static int prev = -1;
    static uint32_t cool = 0;
    uint16_t x[2], y[2];
    uint8_t cnt = 0;
    touch_last_sample(x, y, &cnt);
    if (cnt != 2) {
        prev = -1;
        return;
    }
    int d = abs((int)x[0] - (int)x[1]) + abs((int)y[0] - (int)y[1]);
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (prev >= 0 && (int)(now - cool) > 600) {
        if (d - prev > 50 || prev - d > 50) {
            // Anchor: midpoint between the two fingers.
            double lat, lon;
            screen_to_geo(((int)x[0] + (int)x[1]) / 2,
                          ((int)y[0] + (int)y[1]) / 2, &lat, &lon);
            set_zoom_at(s_zoom + (d > prev ? 1 : -1), lat, lon);
            cool = now;
        }
    }
    prev = d;
}

void map_view_destroy(void)
{
    s_ready = false;
    s_marker = NULL;  // children die with the screen
    s_btn_in = s_btn_out = NULL;
    s_btn_center = NULL;
    s_have_fix = false;
    s_have_stored = false;
    s_scale = 0;
    // NOTE: s_map intentionally NOT freed (app singleton for the loader).
    // Buffers persist; returning to the map is instant.
}

bool map_view_is_live(void)
{
    return s_ready && s_map != NULL;
}
