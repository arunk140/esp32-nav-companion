#include "map/map_view.h"
#include "map_tiles.h"
#include "display/lvgl9_port.h"  // touch_last_sample (sole touch reader is LVGL)
#include "settings.h"            // last known position (gray marker)
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>

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

static void reload_all(void);  // defined below (needs s_imgs, created in map_view_create)

// Consume accumulated drag: move container, shift grid by whole tiles,
// reload changed tiles. Runs at most every 33 ms from map_view_poll().
static void map_view_apply(void)
{
    if (!s_acc_x && !s_acc_y) return;
    int dx = s_acc_x, dy = s_acc_y;
    s_acc_x = s_acc_y = 0;
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
        // New grid-centre geo for later zoom ops.
        map_tiles_tile_xy_to_gps(s_map, s_gx + GRID / 2.0, s_gy + GRID / 2.0,
                                 &s_clat, &s_clon);
        reload_all();
    }
}

static void reload_all(void)
{
    // NOTE: only called from create path (images exist) and map_view_apply
    // (gated by s_ready in map_view_poll). No guard needed here.
    int64_t t0 = esp_timer_get_time();
    int fails = 0, fresh = 0;
    for (int row = 0; row < GRID; row++) {
        for (int col = 0; col < GRID; col++) {
            int i = row * GRID + col;
            bool was_resident =
                map_tiles_slot_loaded(s_map, i, s_gx + col, s_gy + row);
            if (map_tiles_load_tile_if_changed(s_map, i, s_gx + col, s_gy + row)) {
                if (!was_resident) fresh++;
                lv_image_set_src(s_imgs[i], map_tiles_get_image(s_map, i));
            } else {
                lv_image_set_src(s_imgs[i], NULL);  // gap shows box bg
                fails++;
            }
        }
    }
    if (fails) ESP_LOGD(TAG, "%d tiles missing at z%d origin %d,%d", fails, s_zoom, s_gx, s_gy);
    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    if (ms > 20) ESP_LOGI(TAG, "reload %lld ms (fresh %d)", (long long)ms, fresh);
}

// Recenters grid so (lat,lon) sits at grid centre. Full reload (changed only).
static void recenter(double lat, double lon)
{
    s_clat = lat;
    s_clon = lon;
    map_tiles_set_center_from_gps(s_map, lat, lon);
    map_tiles_get_position(s_map, &s_gx, &s_gy);
    s_px = -CENTER_PX;
    s_py = -CENTER_PX;
    lv_obj_set_pos(s_box, s_px, s_py);
    reload_all();
}

static void set_zoom(int z)
{
    if (z < ZOOM_MIN) z = ZOOM_MIN;
    if (z > ZOOM_MAX) z = ZOOM_MAX;
    if (z == s_zoom) return;
    s_zoom = z;
    map_tiles_set_zoom(s_map, z);
    recenter(s_clat, s_clon);
    ESP_LOGI(TAG, "zoom %d", z);
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

static void btn_zoom_in(lv_event_t *e) { (void)e; set_zoom(s_zoom + 1); }
static void btn_zoom_out(lv_event_t *e) { (void)e; set_zoom(s_zoom - 1); }
static void recenter_preferred(void)
{
    // Live fix > stored fix > browse default.
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
    // Outside the inner tile? Recenter so the fix sits at grid centre.
    if (fx < s_gx + 1 || fx >= s_gx + 2 || fy < s_gy + 1 || fy >= s_gy + 2) {
        recenter(lat, lon);
        map_tiles_gps_to_tile_xy(s_map, lat, lon, &fx, &fy);
        // Grid jumped: snap, don't animate across the world.
        s_mrx = fx;
        s_mry = fy;
    }
    s_mtx = fx;
    s_mty = fy;
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
    map_tiles_config_t cfg = {
        .base_path = "/sdcard",
        .tile_folders = {folders[0]},
        .tile_type_count = 1,
        .default_zoom = DEFAULT_ZOOM,
        .use_spiram = true,
        .default_tile_type = 0,
        .grid_cols = GRID,
        .grid_rows = GRID,
    };
    s_map = map_tiles_init(&cfg);
    if (!s_map) {
        lv_obj_t *l = lv_label_create(parent);
        lv_label_set_text(l, "No map: SD missing?");
        lv_obj_center(l);
        ESP_LOGE(TAG, "map_tiles_init failed");
        return;
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
    lv_obj_t *bi = mk_btn(parent, "+", 404, 176);
    lv_obj_add_event_cb(bi, btn_zoom_in, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bo = mk_btn(parent, "-", 404, 240);
    lv_obj_add_event_cb(bo, btn_zoom_out, LV_EVENT_CLICKED, NULL);
    lv_obj_t *br = lv_button_create(parent);
    lv_obj_set_size(br, 64, 44);
    lv_obj_set_pos(br, 208, 412);
    lv_obj_t *rl = lv_label_create(br);
    lv_label_set_text(rl, "C");  // recenter; ASCII only (LVGL font has no symbols)
    lv_obj_center(rl);
    lv_obj_add_event_cb(br, btn_recenter, LV_EVENT_CLICKED, NULL);

    recenter(DEFAULT_LAT, DEFAULT_LON);
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
        if (d - prev > 50) {
            set_zoom(s_zoom + 1);
            cool = now;
        } else if (prev - d > 50) {
            set_zoom(s_zoom - 1);
            cool = now;
        }
    }
    prev = d;
}

void map_view_destroy(void)
{
    s_ready = false;
    s_marker = NULL;  // children die with the screen
    s_have_fix = false;
    s_have_stored = false;
    if (s_map) {
        map_tiles_cleanup(s_map);
        s_map = NULL;
    }
}

bool map_view_is_live(void)
{
    return s_ready && s_map != NULL;
}
