#include "map/map_view.h"
#include "map/loader.h"
#include "map_tiles.h"
#include "display/lvgl9_port.h"  // touch_last_sample (sole touch reader is LVGL)
#include "settings.h"            // last known position (gray marker)
#include "esp_log.h"
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
#define HALF (SCR / 2)
#define MARK_R 10  // marker radius (the object is a 2*MARK_R square)
#define MAP_BG 0x1a1a1a  // gaps and screen background

// --- Motion filter ---------------------------------------------------------
// Everything drawn is an eased value, never a raw fix. The phone delivers
// 1 Hz (and ~10.5 s clumps under Doze), so applying fixes verbatim gives a
// visible stutter: at z16 one 15 m fix step is 6.6 px, and the old 15 m
// jitter deadband turned that into a leap that recurred every second at
// 50 km/h. A time-constant ease fixes both cases -- motion becomes
// continuous, wander becomes sub-pixel -- with no deadband to run away.
#define SMOOTH_TAU_S 0.35   // exponential time constant (~95% settled in 1 s)
// Below this the remaining motion is a fraction of a pixel: invisible, and
// stopping matters. An exact-convergence test (1e-9 tile units) would keep
// the ease -- and the redraw it schedules -- running for hundreds of polls.
#define EASE_EPS_PX 0.05
// Beyond this the fix is a teleport (Doze batch, reconnect, a first fix far
// from the browse default). Snap instead of sliding the map across screen.
#define SNAP_M 150.0
// Hard cap on on-screen travel per poll, in pixels, so one huge jump can
// never outrun the redraw. 480 px/s is ~40x a road vehicle at z16.
#define MAX_PAN_PX_S 480
// Longest a poll may be assumed to be (clamped), so a stalled frame cannot
// turn one ease step into a visible leap.
#define MAX_DT_S 0.25

// --- Tile loading ----------------------------------------------------------
#define LOAD_STALL_MS 3000  // > 9 sequential reads; past this, re-arm the slot
#define SLOT_TRIES 3         // give up on a tile that is not in the download
#define ZOOM_COOL_MS 600     // one pinch step per this long
#define DRAG_APPLY_MS 33     // 30 Hz drag application

static map_tiles_handle_t s_map;
static lv_obj_t *s_box;              // 768x768 tile container
static lv_obj_t *s_imgs[GRID * GRID];
static int s_gx, s_gy;               // grid origin (top-left tile) at s_zoom
static bool s_grid_init;             // first grid_sync must not rotate slots
static int s_grid_zoom = -1;         // zoom the resident slot pixels belong to
static int s_px, s_py;               // container position on screen
static int s_zoom = DEFAULT_ZOOM;
static bool s_ready;                 // set at end of map_view_create

// All geographic state lives in ABSOLUTE fractional tile coordinates at
// s_zoom. That is what makes a grid shift and a zoom both trivial: neither
// invalidates these numbers, so there is no frame_shift() to get wrong.
// s_m* is what is drawn; s_t* is where it is heading.
static double s_mcx, s_mcy;          // smoothed map centre
static double s_tmcx, s_tmcy;        // map centre target
static double s_mfx, s_mfy;          // smoothed position fix
static double s_tfx, s_tfy;          // fix target
static bool s_have_geom;             // s_mfx/s_mfy are valid

static double s_fix_lat, s_fix_lon;  // last accepted raw live fix
static bool s_have_fix;
static double s_stored_lat, s_stored_lon;  // last persisted fix (gray marker)
static bool s_have_stored;
static lv_obj_t *s_marker;
// SNAP_M expressed in tile units at the current zoom. Cached: it only depends
// on the zoom and the fix latitude, and recomputing it means a software
// double cos() in the per-poll path.
static double s_snap_t;

// Follow mode: live fixes recenter the map. Any manual drag/zoom clears it;
// Center re-engages it. Starts ON so a fresh connect follows until touched.
static bool s_follow = true;

// Drag: the poll samples the shared touch snapshot and feeds deltas here;
// map_view_apply() consumes them at 30 Hz so SD reloads never run in the
// gesture path and the map pans smoothly between tile shifts.
static int s_acc_x, s_acc_y;
static bool s_pan_down;
static lv_point_t s_pan_last = {0, 0};
static int s_pinch_prev = -1;
static uint32_t s_last_apply;

// Per-slot tile loading state. This replaces the old s_pending/s_qpend pair,
// which counted requests the loader had silently dropped: a dropped slot
// could never complete. The state is an explicit enum so a slot cannot be
// re-armed into a loop.
typedef enum {
    WANT_OK,      // holds the wanted tile
    WANT_LOAD,    // wanted, not enqueued (or the queue refused it)
    WANT_BUSY,    // enqueued, awaiting a completion
    WANT_GIVEUP,  // not on the card; show the gap, stop asking
} want_t;

static int s_wtx[GRID * GRID], s_wty[GRID * GRID];
static want_t s_want[GRID * GRID];
static uint32_t s_wt0[GRID * GRID];   // enqueue time (stall detection)
static uint8_t s_wtry[GRID * GRID];   // attempts so far
static int s_wopen;                   // slots not in WANT_OK (O(1) idle test)
static uint32_t s_gen;      // invalidates in-flight loads on a new batch
static uint32_t s_zoom_cool;

static void place(void);
static void grid_request(void);
static void map_update_center_btn(void);

// m/px for the current zoom (Web Mercator). Used to express the noise floor
// and the snap distance in metres without hard-coding a zoom.
static double mpp(double lat_deg)
{
    return 156543.03392 * cos(lat_deg * M_PI / 180.0) / (double)(1 << s_zoom);
}

static double tiles_per_m(double m, double lat_deg)
{
    return m / mpp(lat_deg);
}

// Refresh the cached SNAP_M-in-tiles. Call whenever the zoom or the fix
// latitude changes -- i.e. on a fix and on a zoom, never per poll.
static void snap_scale_update(double lat_deg)
{
    s_snap_t = tiles_per_m(SNAP_M, lat_deg);
}

// One exponential ease step toward (tx, ty), in tile units. Frame-rate
// independent (the old fixed 0.02-per-poll factor moved the dot further on a
// slow frame, which is its own kind of stutter) and speed-capped so no single
// step can outrun the redraw. Returns true if anything moved.
static bool ease(double *x, double *y, double tx, double ty, double dt_s)
{
    double dx = tx - *x, dy = ty - *y;
    double d = sqrt(dx * dx + dy * dy);
    if (d * TILE < EASE_EPS_PX) {  // converged: snap and stop doing work
        bool moved = (*x != tx || *y != ty);
        *x = tx;
        *y = ty;
        return moved;
    }
    double step = d * (1.0 - exp(-dt_s / SMOOTH_TAU_S));
    double cap = MAX_PAN_PX_S * dt_s / TILE;
    if (step > cap) step = cap;
    *x += dx * (step / d);
    *y += dy * (step / d);
    return true;
}

// Grid origin follows the smoothed centre. Slots are rotated so the tiles
// still on screen stay resident, so a one-tile pan costs 3 SD reads, not 9.
//
// NOT across a zoom: after a zoom each slot already holds the right *area* at
// the wrong scale (grid cell (r,c) needed (gx+col, gy+row) before and still
// needs it), so leaving the pixels in place gives a correct "sharpen" for
// free. Rotating would shuffle every tile to the wrong cell.
static void grid_sync(void)
{
    int ngx = (int)floor(s_mcx) - GRID / 2;
    int ngy = (int)floor(s_mcy) - GRID / 2;
    if (s_grid_init && ngx == s_gx && ngy == s_gy) return;
    int dx = s_grid_init ? ngx - s_gx : 0;
    int dy = s_grid_init ? ngy - s_gy : 0;
    s_grid_init = true;
    s_gx = ngx;
    s_gy = ngy;
    if ((dx || dy) && s_grid_zoom == s_zoom)
        map_tiles_shift_slots(s_map, dx, dy);
    // Keep the component's own origin in sync too: it is what the geo
    // helpers and set_center_from_gps() read.
    map_tiles_set_position(s_map, s_gx, s_gy);
    grid_request();
}

// Publish the smoothed state: grid origin, container position, marker.
// The map centre is what lands on screen centre, so the container offset is
// derived from it and can never drift out of range.
static void place(void)
{
    grid_sync();
    s_px = HALF - (int)((s_mcx - s_gx) * TILE);
    s_py = HALF - (int)((s_mcy - s_gy) * TILE);
    lv_obj_set_pos(s_box, s_px, s_py);
    if (s_marker) {
        // s_marker is a child of s_box, so its position is relative to the
        // container: screen = (fix - origin) * TILE + container offset. In
        // follow mode this lands the dot on screen centre automatically,
        // without any per-fix special case.
        lv_obj_set_pos(s_marker, (int)((s_mfx - s_gx) * TILE) + s_px - MARK_R,
                                    (int)((s_mfy - s_gy) * TILE) + s_py - MARK_R);
    }
}

// Set a slot's state, keeping s_wopen (slots not yet settled) in step. The
// poll uses s_wopen as an O(1) "is there any loading at all" test so a settled
// map costs one integer compare per poll instead of nine slot queries.
static void slot_set(int i, want_t st)
{
    bool was_open = s_want[i] != WANT_OK;
    bool is_open = st != WANT_OK;
    s_want[i] = st;
    if (was_open && !is_open) s_wopen--;
    else if (!was_open && is_open) s_wopen++;
}

// Advance one slot's load state. WANT_LOAD is the only state that can
// change here, and it always leaves as either WANT_OK or WANT_BUSY -- except
// when the loader queue is full, where it stays WANT_LOAD and is retried on
// the next poll. A slot that exhausts SLOT_TRIES becomes WANT_GIVEUP and is
// never re-armed, so a tile missing from the download does not spin (and
// does not re-invalidate a 256 px image 100 times a second).
static void slot_step(int i)
{
    if (s_want[i] != WANT_LOAD) return;
    if (map_tiles_slot_loaded(s_map, i, s_wtx[i], s_wty[i])) {
        // Resident already (rotated in, or loaded earlier): just link it.
        lv_image_set_src(s_imgs[i], map_tiles_get_image(s_map, i));
        slot_set(i, WANT_OK);
        return;
    }
    if (s_wtry[i] >= SLOT_TRIES) {
        lv_image_set_src(s_imgs[i], NULL);
        slot_set(i, WANT_GIVEUP);
        return;
    }
    if (!loader_enqueue(i, s_zoom, s_wtx[i], s_wty[i], s_gen)) return;
    s_wtry[i]++;
    s_wt0[i] = xTaskGetTickCount() * portTICK_PERIOD_MS;
    slot_set(i, WANT_BUSY);
}

// Record what each slot should show and kick off the gaps. Tiles already
// resident are linked immediately; the rest go to the loader task. Anything
// not yet loaded keeps showing its previous pixels until replaced, so a pan
// or a zoom never blanks the screen.
static void grid_request(void)
{
    s_gen++;
    s_grid_zoom = s_zoom;
    for (int row = 0; row < GRID; row++) {
        for (int col = 0; col < GRID; col++) {
            int i = row * GRID + col;
            s_wtx[i] = s_gx + col;
            s_wty[i] = s_gy + row;
            s_wtry[i] = 0;
            slot_set(i, WANT_LOAD);
        }
    }
    for (int i = 0; i < GRID * GRID; i++) slot_step(i);
}

// Drain loader completions: memcpy scratch into the slot buffer (frame-atomic:
// only the GUI task touches LVGL + slot buffers), re-link, release scratch.
// Drops stale generations. Every completion re-evaluates its slot so the
// want is either satisfied, retried, or given up on.
static void grid_drain(void)
{
    load_done_t d;
    while (loader_poll_done(&d)) {
        if (d.gen != s_gen || d.slot < 0 || d.slot >= GRID * GRID) {
            if (d.scratch) loader_release(d.scratch);
            continue;  // superseded by a newer grid_request
        }
        if (d.scratch) {
            uint8_t *buf = map_tiles_slot_ensure(s_map, d.slot);
            if (buf) {
                memcpy(buf, d.scratch, LOADER_TILE_BYTES);
                map_tiles_slot_commit(s_map, d.slot, d.x, d.y, d.z);
                lv_image_set_src(s_imgs[d.slot],
                                 map_tiles_get_image(s_map, d.slot));
                lv_obj_invalidate(s_imgs[d.slot]);
            }
            loader_release(d.scratch);
        } else {
            // Not on the card (or a short read). Show the gap now; s_wtry
            // decides whether to ask again.
            lv_image_set_src(s_imgs[d.slot], NULL);
            lv_obj_invalidate(s_imgs[d.slot]);
        }
        slot_set(d.slot, WANT_LOAD);  // -> WANT_OK, or retry, or give up
        slot_step(d.slot);
    }
}

// Reconcile: the completion queue drops rather than blocks, and a dropped
// completion would leave a slot stuck in WANT_BUSY forever. Re-arm anything
// that has been waiting implausibly long (9 sequential reads is ~1.2 s worst
// case, so LOAD_STALL_MS leaves generous headroom).
static void grid_reconcile(uint32_t now)
{
    for (int i = 0; i < GRID * GRID; i++) {
        if (s_want[i] == WANT_BUSY && (int)(now - s_wt0[i]) >= LOAD_STALL_MS)
            slot_set(i, WANT_LOAD);
        slot_step(i);
    }
}

// Re-derive the marker's tile coords from the last known geo position. They
// are absolute tile units, which change with the zoom, so a zoom MUST
// reproject them: carrying the old numbers left the dot about 1.2e7 px off
// the grid (it vanished in follow mode, and streaked across the screen in
// browse mode while the ease chased it).
static void fix_reproject(void)
{
    double lat, lon;
    if (s_have_fix) { lat = s_fix_lat; lon = s_fix_lon; }
    else if (s_have_stored) { lat = s_stored_lat; lon = s_stored_lon; }
    else return;
    map_tiles_gps_to_tile_xy(s_map, lat, lon, &s_tfx, &s_tfy);
    s_mfx = s_tfx;
    s_mfy = s_tfy;
    s_have_geom = true;
}

static void ensure_marker(bool live)
{
    if (!s_marker) {
        s_marker = lv_obj_create(s_box);
        lv_obj_set_size(s_marker, 2 * MARK_R, 2 * MARK_R);
        lv_obj_set_style_radius(s_marker, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_marker, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(s_marker, lv_color_white(), 0);
        lv_obj_set_style_border_width(s_marker, 3, 0);
        lv_obj_remove_flag(s_marker, LV_OBJ_FLAG_SCROLLABLE);
        // Must not eat the press, or there is a dead spot on the dot.
        lv_obj_set_clickable(s_marker, false);
    }
    // Blue = live fix, gray = stored fix with no phone.
    lv_obj_set_style_bg_color(s_marker, lv_color_hex(live ? 0x2563eb : 0x6b7280), 0);
}

// Put the map centre on a geo point immediately (no easing): a recenter is a
// command, not motion.
static void centre_snap(double lat, double lon)
{
    map_tiles_gps_to_tile_xy(s_map, lat, lon, &s_tmcx, &s_tmcy);
    s_mcx = s_tmcx;
    s_mcy = s_tmcy;
    place();
}

void map_accum(int dx, int dy)
{
    s_acc_x += dx;
    s_acc_y += dy;
}

// Consume accumulated drag at 30 Hz. Direct manipulation: the map follows the
// finger with no easing, so the centre and its target move together.
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
    // Finger right -> map right -> centre left.
    double mx = (double)dx / TILE, my = (double)dy / TILE;
    s_mcx -= mx; s_tmcx -= mx;
    s_mcy -= my; s_tmcy -= my;
    place();
}

// Is this screen point on the map (s_box or one of its children) rather than
// on a button? The +/-/C buttons are siblings drawn on top, so panning on raw
// coordinates would drag the map while the user is aiming at "+".
static bool press_on_map(int sx, int sy)
{
    lv_point_t p = {sx, sy};
    lv_obj_t *hit = lv_indev_search_obj(lv_screen_active(), &p);
    for (lv_obj_t *o = hit; o; o = lv_obj_get_parent(o))
        if (o == s_box) return true;
    return false;
}

// Pan: derived from the shared touch snapshot inside the poll, NOT from LVGL
// press events. Events are fragile on this screen: PRESSED has to reach
// s_box, and in LVGL 9 PRESSED/PRESSING/RELEASED do not bubble, so a
// clickable child swallows the gesture and the pan silently does nothing.
// The poll runs at a fixed 100 Hz, which also makes the pan speed
// frame-rate independent.
//
// touch_last_sample() reads the snapshot lvgl9_port already took; it is not a
// second controller read (see lvgl9_port.c -- get_coordinates invalidates the
// sample and starves LVGL's indev).
static void pan_from_touch(void)
{
    uint16_t x[2], y[2];
    uint8_t cnt = 0;
    touch_last_sample(x, y, &cnt);
    if (cnt != 1) {
        // No contact, or two+ (that's the pinch path). Also drop any pan
        // accumulated earlier in this gesture.
        s_pan_down = false;
        return;
    }
    if (!s_pan_down) {
        if (!press_on_map(x[0], y[0])) return;  // started on a button
        s_pan_down = true;
        s_pan_last.x = x[0];
        s_pan_last.y = y[0];
        return;
    }
    int dx = (int)x[0] - s_pan_last.x;
    int dy = (int)y[0] - s_pan_last.y;
    s_pan_last.x = x[0];
    s_pan_last.y = y[0];
    if (dx || dy) map_accum(dx, dy);
}

static lv_obj_t *s_btn_in, *s_btn_out, *s_btn_center;

// Center button is only useful when NOT following: hidden while centered,
// reappears on any manual drag/zoom.
static void map_update_center_btn(void)
{
    if (!s_btn_center) return;
    if (s_follow) lv_obj_add_flag(s_btn_center, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_btn_center, LV_OBJ_FLAG_HIDDEN);
}

// Gray out zoom buttons at the limits (ASCII-safe, no icon fonts needed).
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

// Geo under a screen point (the zoom anchor). Inverts the container offset:
// screen (sx,sy) -> box (sx-px, sy-py) -> fractional tile -> GPS.
static void screen_to_geo(int sx, int sy, double *lat, double *lon)
{
    double fx = s_gx + (sx - s_px) / (double)TILE;
    double fy = s_gy + (sy - s_py) / (double)TILE;
    map_tiles_tile_xy_to_gps(s_map, fx, fy, lat, lon);
}

// Zoom anchored on a fixed geo point: that point keeps its screen position.
//
// Tile coordinates scale exactly by k = 2^dz, and a tile is TILE px at every
// zoom (the geographic span per tile changes, the pixel size does not). So
// preserving the anchor's screen offset means preserving its offset from the
// map centre, in tile units:
//     (ox*k - new_centre) * TILE = (ox - centre) * TILE
//  => new_centre = centre + ox*(k - 1)
// Anchoring on the map centre (ox == centre) degenerates to new_centre =
// ox*k, which is why zooming at screen centre holds the view exactly.
//
// There is deliberately NO scaled preview of the old content. Scaling s_box
// cannot work here: it is 768 px on a 480 px screen, so it only covers the
// display by being at least as big as it, and a 0.5x zoom-out preview
// shrank it to 384 px -- exposing the (theme-less, hence white) screen
// background as a white flash. Instead the slots keep their pixels, which
// are already the right area at the previous scale, so the map visibly
// sharpens once the new tiles land.
static void set_zoom_at(int z, double lat, double lon)
{
    if (z < ZOOM_MIN) z = ZOOM_MIN;
    if (z > ZOOM_MAX) z = ZOOM_MAX;
    // Nothing to do: return without recentering. The old early-return still
    // called recenter(), so tapping "+" at max zoom jumped the map up to
    // 128 px per axis for no reason.
    if (z == s_zoom) return;

    // Anchor in the OLD zoom's tile units, before the component is told.
    double ox, oy;
    map_tiles_gps_to_tile_xy(s_map, lat, lon, &ox, &oy);
    double k = ldexp(1.0, z - s_zoom);

    // Manual zoom breaks follow mode (re-lock via Center).
    if (s_follow) {
        s_follow = false;
        map_update_center_btn();
    }

    s_zoom = z;
    settings_set_zoom(z);  // persisted; zoom changes are rare (GUI task: safe)
    map_tiles_set_zoom(s_map, z);
    s_tmcx = s_mcx += ox * (k - 1.0);  // a zoom is a jump, not motion
    s_tmcy = s_mcy += oy * (k - 1.0);
    fix_reproject();
    // m/px changed with the zoom, so the cached snap distance must too.
    if (s_have_fix) snap_scale_update(s_fix_lat);
    else if (s_have_stored) snap_scale_update(s_stored_lat);
    place();  // may shift the grid and start the new tile batch
    map_update_zoom_btns();
    ESP_LOGI(TAG, "zoom %d", z);
}

static void btn_zoom_in(lv_event_t *e)
{
    (void)e;
    double lat, lon;
    screen_to_geo(HALF, HALF, &lat, &lon);  // anchor: screen centre
    set_zoom_at(s_zoom + 1, lat, lon);
}

static void btn_zoom_out(lv_event_t *e)
{
    (void)e;
    double lat, lon;
    screen_to_geo(HALF, HALF, &lat, &lon);  // anchor: screen centre
    set_zoom_at(s_zoom - 1, lat, lon);
}

static void recenter_preferred(void)
{
    // Live fix > stored fix > browse default. Re-locks follow mode.
    s_follow = true;
    map_update_center_btn();
    if (s_have_fix) centre_snap(s_fix_lat, s_fix_lon);
    else if (s_have_stored) centre_snap(s_stored_lat, s_stored_lon);
    else centre_snap(DEFAULT_LAT, DEFAULT_LON);
}

static void btn_recenter(lv_event_t *e)
{
    (void)e;
    recenter_preferred();
}

void map_view_set_zoom(int z)
{
    // Anchor on the map centre, which is the screen centre by construction.
    // Going through screen_to_geo() means a remote zoom no longer snaps the
    // view back to a cached pre-drag centre.
    double lat, lon;
    screen_to_geo(HALF, HALF, &lat, &lon);
    set_zoom_at(z, lat, lon);
}

void map_view_recenter_default(void)
{
    // CMD 0x01: live fix > stored fix > browse default.
    recenter_preferred();
}

void map_view_set_position(double lat, double lon, bool live)
{
    if (!s_ready) return;
    if (live) {
        s_fix_lat = lat;
        s_fix_lon = lon;
        s_have_fix = true;
    }
    // The raw fix is always the target; the ease does the filtering. A
    // deadband here would be a stutter generator, not a filter: it freezes
    // the smoothed value while the vehicle keeps moving, so the target walks
    // away and the map leaps when the gate finally reopens. (The old 15 m
    // gate did exactly that -- a 1 Hz 6.6 px leap at 50 km/h.)
    map_tiles_gps_to_tile_xy(s_map, lat, lon, &s_tfx, &s_tfy);
    snap_scale_update(lat);
    if (!s_have_geom) {
        s_mfx = s_tfx;
        s_mfy = s_tfy;
        s_have_geom = true;
    } else {
        // Teleport (Doze batch, reconnect): snap. Easing a 500 m jump reads
        // as a broken map, not as motion.
        double dx = s_tfx - s_mfx, dy = s_tfy - s_mfy;
        if (dx * dx + dy * dy > s_snap_t * s_snap_t) {
            s_mfx = s_tfx;
            s_mfy = s_tfy;
        }
    }
    if (!live) {
        // A stored fix is a point, not a stream: place it, do not ease it.
        s_mfx = s_tfx;
        s_mfy = s_tfy;
    }
    ensure_marker(live);
    place();
}

// Pinch: consume the shared touch snapshot (see lvgl9_port). LVGL's indev is
// the sole I2C reader; reading the controller here would invalidate LVGL's
// sample. >50 px manhattan change + 600 ms cooldown, anchored on the
// midpoint between the two fingers.
static void pinch(uint32_t now)
{
    uint16_t x[2], y[2];
    uint8_t cnt = 0;
    touch_last_sample(x, y, &cnt);
    if (cnt != 2) {
        s_pinch_prev = -1;
        s_acc_x = s_acc_y = 0;  // drop any pan accumulated mid-pinch
        return;
    }
    int d = abs((int)x[0] - (int)x[1]) + abs((int)y[0] - (int)y[1]);
    if (s_pinch_prev < 0) { s_pinch_prev = d; return; }
    int dd = d - s_pinch_prev;
    if ((dd > 50 || dd < -50) && (int)(now - s_zoom_cool) > ZOOM_COOL_MS) {
        double lat, lon;
        screen_to_geo(((int)x[0] + (int)x[1]) / 2,
                      ((int)y[0] + (int)y[1]) / 2, &lat, &lon);
        set_zoom_at(s_zoom + (dd > 0 ? 1 : -1), lat, lon);
        s_zoom_cool = now;
        s_pinch_prev = d;  // re-baseline: one spread, one zoom step
        return;
    }
    s_pinch_prev = d;
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
    // No theme is applied on this display, so the screen background is LVGL's
    // default opaque WHITE. Paint it the map's gap colour: any area not
    // covered by tiles (a not-yet-loaded grid, a one-pixel edge) then reads
    // as "no tile" instead of flashing white.
    lv_obj_set_style_bg_color(parent, lv_color_hex(MAP_BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    const char *folders[] = {"street_map"};
    // Restore persisted zoom (settings clamps it; clamp again so a bad NVS
    // value can never leave us on a zoom with no tiles on the card).
    s_zoom = settings_get_zoom();
    if (s_zoom < ZOOM_MIN) s_zoom = ZOOM_MIN;
    if (s_zoom > ZOOM_MAX) s_zoom = ZOOM_MAX;
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

    for (int i = 0; i < GRID * GRID; i++) {
        s_wtx[i] = INT32_MIN;
        s_wty[i] = INT32_MIN;
        s_wtry[i] = 0;
        s_wt0[i] = 0;
        s_want[i] = WANT_GIVEUP;  // nothing requested yet
    }
    s_wopen = 0;
    // Clean slate: show_map() runs after map_view_destroy(), but also on a
    // cold boot, so do not rely on the destroy side alone.
    s_grid_init = false;
    s_grid_zoom = -1;
    s_have_geom = false;
    s_have_fix = false;
    s_have_stored = false;
    s_snap_t = 0.0;
    s_follow = true;
    s_acc_x = s_acc_y = 0;
    s_pan_down = false;
    s_pan_last.x = s_pan_last.y = 0;
    s_pinch_prev = -1;
    s_last_apply = 0;
    s_zoom_cool = 0;
    s_gen++;

    s_box = lv_obj_create(parent);
    lv_obj_set_size(s_box, GRID * TILE, GRID * TILE);
    lv_obj_set_style_pad_all(s_box, 0, 0);
    lv_obj_set_style_border_width(s_box, 0, 0);
    lv_obj_set_style_radius(s_box, 0, 0);
    lv_obj_set_style_bg_color(s_box, lv_color_hex(MAP_BG), 0);  // gaps
    lv_obj_set_clickable(s_box, true);

    for (int row = 0; row < GRID; row++) {
        for (int col = 0; col < GRID; col++) {
            int i = row * GRID + col;
            s_imgs[i] = lv_image_create(s_box);
            lv_obj_set_pos(s_imgs[i], col * TILE, row * TILE);
            // LOAD-BEARING: a clickable tile wins the indev hit test, and
            // LVGL 9 does not bubble PRESSED/PRESSING/RELEASED, so a press
            // landing on a tile would never reach the map.
            lv_obj_set_clickable(s_imgs[i], false);
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

    centre_snap(DEFAULT_LAT, DEFAULT_LON);
    map_update_zoom_btns();
    map_update_center_btn();  // hidden initially (following)
    // Ready before the stored-fix call below: map_view_set_position() gates
    // on s_ready, so setting it afterwards silently dropped the boot marker
    // and the map showed no dot at all until the first live fix.
    s_ready = true;
    // Boot to the last known fix if there is one (gray = stale, no phone).
    int32_t ll_lat = 0, ll_lon = 0;
    if (settings_get_last_pos(&ll_lat, &ll_lon)) {
        s_stored_lat = ll_lat / 1e7;
        s_stored_lon = ll_lon / 1e7;
        s_have_stored = true;
        centre_snap(s_stored_lat, s_stored_lon);
        map_view_set_position(s_stored_lat, s_stored_lon, false);
    }
    ESP_LOGI(TAG, "browse map ready (%s)",
             s_have_stored ? "stored fix" : "no location");
}

void map_view_poll(void)
{
    if (!s_ready) return;
    static uint32_t last;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    double dt = (int)(now - last) / 1000.0;
    last = now;
    if (dt < 0.001) dt = 0.001; else if (dt > MAX_DT_S) dt = MAX_DT_S;

    // Gesture first, so a pan in progress is visible in this same frame.
    pan_from_touch();
    if ((int)(now - s_last_apply) > DRAG_APPLY_MS) {
        s_last_apply = now;
        map_view_apply();
    }
    pinch(now);

    // Follow mode: the map centre chases the smoothed fix, so the dot and
    // the map are the same eased value and can never disagree. This replaces
    // the old code, which snapped s_mrx to the raw fix and made the ease
    // dead — the 1 Hz stutter was the raw fix applied verbatim, twice.
    // Guarded on s_have_geom: before any fix, s_tfx is still (0,0), and
    // following it would send the map to null island.
    bool moved = false;
    if (s_follow && s_have_geom) {
        s_tmcx = s_tfx;
        s_tmcy = s_tfy;
        // Teleport: move the map with the dot rather than sliding it (a Doze
        // batch, a reconnect, or a first fix far from the browse default).
        double dx = s_tmcx - s_mcx, dy = s_tmcy - s_mcy;
        if (dx * dx + dy * dy > s_snap_t * s_snap_t) {
            s_mcx = s_tmcx;
            s_mcy = s_tmcy;
            moved = true;
        }
    }
    if (ease(&s_mcx, &s_mcy, s_tmcx, s_tmcy, dt)) moved = true;
    if (s_have_geom && ease(&s_mfx, &s_mfy, s_tfx, s_tfy, dt)) moved = true;
    // Only touch LVGL when something actually changed: place() invalidates
    // the 768px container, which at the poll rate is a redraw storm.
    if (moved) place();

    // Idle fast paths: one integer test each instead of queue peeks and nine
    // slot queries per tick. A settled map must cost almost nothing, or the
    // 100 Hz poll starves IDLE1 and trips the task watchdog.
    if (loader_active()) grid_drain();
    if (s_wopen) grid_reconcile(now);
}

void map_view_destroy(void)
{
    s_ready = false;
    s_marker = NULL;  // children die with the screen
    s_btn_in = s_btn_out = NULL;
    s_btn_center = NULL;
    s_have_fix = false;
    s_have_stored = false;
    s_have_geom = false;
    s_snap_t = 0.0;
    s_follow = true;
    s_acc_x = s_acc_y = 0;
    s_last_apply = 0;
    s_pan_down = false;
    s_pan_last.x = s_pan_last.y = 0;
    s_pinch_prev = -1;
    s_gen++;  // every in-flight load is now stale
    for (int i = 0; i < GRID * GRID; i++) s_want[i] = WANT_GIVEUP;
    s_wopen = 0;
    s_grid_zoom = -1;
    // NOTE: s_map intentionally NOT freed (app singleton for the loader).
    // Buffers persist; returning to the map is instant.
}

bool map_view_is_live(void)
{
    return s_ready && s_map != NULL;
}
