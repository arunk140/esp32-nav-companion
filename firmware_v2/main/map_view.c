#include "map_view.h"
#include "ble_service.h"
#include "nav_view.h"
#include "board.h"
#include "display.h"
#include "ui_common.h"
#include "sdcard.h"
#include "settings.h"
#include "tiles.h"
#include "touch.h"
#include "esp_attr.h"
#include "esp_rom_sys.h"
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
#define COL_LIVE 0x1A73E8     // position marker, dark map
#define COL_LIVE_L 0x174EA6   // position marker, light map: darker than the route
#define COL_STALE 0x8A8F96
#define COL_DEST 0xE8710A
#define COL_ROUTE_L 0x1A73E8   // Google-blue route line, light map
#define COL_ROUTE_D 0x8AB4F8   // lighter blue, dark map
// The part of the route already covered.
#define COL_DONE_L 0xA4A9B0     // light map
#define COL_DONE_D 0x5F656D     // dark map
#define COL_DONE_MINI 0x44484E  // minimal display, on black
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
// Route line (Valhalla geometry), assembled from BLE chunks. GUI task only.
#define ROUTE_MAX 512
static int32_t s_route_lat[ROUTE_MAX], s_route_lon[ROUTE_MAX];
static int s_route_n;
static bool s_route_active;
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
static lv_obj_t *s_btn_in, *s_btn_out, *s_btn_center, *s_btn_media, *s_toast;
static uint32_t s_toast_until;
static int s_status_key = -1;
static int s_status_eta_min = -1;   // last ETA minutes drawn (nav pill)
static void (*s_reset_cb)(void);
static void (*s_media_cb)(void);
static void (*s_sleep_cb)(void);
static bool s_sleep_req;

// Minimal display (see map_view.h). It keeps its own zoom: the map's is tied
// to the zoom levels on the SD card, and this mode has no tiles to match.
#define MINI_ZOOM_MIN 12
#define MINI_ZOOM_MAX 18
#define MINI_ZOOM_DEFAULT 17
// Scale on top of zoom 17. CRUISE is the normal view: about 0.7 m per pixel,
// so roughly 210 m of road shows ahead of the arrow. Nearing a turn the view
// pulls back to TURN (half of that, about 420 m ahead), and closes in again
// once the next turn is further than TURN_FAR_M, which means this one is
// behind us. The gap between NEAR and FAR stops it hunting when the distance
// hovers. NEAR sits just past what CRUISE can show, so the pull-back starts as
// the turn is about to come into view.
#define CRUISE_SCALE 1.6
#define TURN_SCALE 0.8
#define TURN_NEAR_M 250
#define TURN_FAR_M 320
#define SCALE_RATE 2.5f       // 1/s, exponential ease
#define SWIPE_MIN_PX 70       // sideways travel that counts as a swipe
#define MINI_AHEAD_PX 60      // the arrow sits this far below the centre
#define MODE_HOLD_MS 900      // long press on the map switches mode
static bool s_minimal;
static int s_zoom_other = MINI_ZOOM_DEFAULT;  // the zoom of the mode not shown
static void (*s_mode_cb)(void);
static bool s_mode_req;
static uint32_t s_press_t0;
static bool s_press_fired;
static int s_prog_shown = -2;
// The minimal display's faces, in swipe order: right goes down this list,
// left goes back up it, and left from the first one opens the music controls.
// The route and the arrival face only exist during a drive.
enum { FACE_ROUTE, FACE_SPEED, FACE_ETA, FACE_PHONE, FACE_N };
static int s_face = FACE_ROUTE;
static bool s_speedo;         // a face other than the route is showing
// The face the user swiped to, or -1 for no choice: then the route shows
// during a drive and the speedometer otherwise. A drive starting or ending
// clears the choice.
static int s_speedo_pref = -1;
// Trip since power-on, counted from the fixes while moving.
static double s_trip_m;
static uint32_t s_trip_moving_ms;
// From the phone: battery, and local time as an epoch taken at s_clock_at.
static int s_phone_batt = -1;
static bool s_phone_charging;
static uint32_t s_clock_epoch, s_clock_at;
static bool s_clock_ok;
static int (*s_bright_cb)(int delta);
static bool s_turn_near;
static uint32_t s_scale_t;
static lv_point_t s_press_pt;
static int32_t s_speedo_key = -1;

static inline int zoom_min(void)
{
    return s_minimal ? MINI_ZOOM_MIN : s_zmin;
}

static inline int zoom_max(void)
{
    return s_minimal ? MINI_ZOOM_MAX : s_zmax;
}

// Overlay colours follow the background, which is black in minimal mode
// whatever the tile theme says.
static inline bool dark_bg(void)
{
    return s_minimal || tiles_dark();
}
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

// Minimal display only: a smooth zoom factor on top of the integer level,
// used to pull back for a turn. 1.0 everywhere else, so tiles are unaffected.
static double s_scale = 1.0;

static inline double world_px(void)
{
    return 256.0 * (double)(1u << s_zoom) * s_scale;
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
#define MARK_RING_W 2
#define MARK_EDGE 2.0f  // white edge around the heading arrow, px
#define MARK_CONE_LEN (36.0f * MARKER_SCALE)
#define MARK_CONE_W (15.0f * MARKER_SCALE)

// One heading-arrow triangle: tip `len` px ahead of (x, y), base corners `w`
// px out at +/-0.55 rad either side of the heading `a`.
static void draw_arrow(lv_layer_t *layer, int32_t x, int32_t y, float a, float len, float w,
                       lv_color_t color)
{
    lv_draw_triangle_dsc_t t;
    lv_draw_triangle_dsc_init(&t);
    t.color = color;
    t.opa = LV_OPA_COVER;
    t.p[0].x = x + sinf(a) * len;
    t.p[0].y = y - cosf(a) * len;
    t.p[1].x = x + sinf(a - 0.55f) * w;
    t.p[1].y = y - cosf(a - 0.55f) * w;
    t.p[2].x = x + sinf(a + 0.55f) * w;
    t.p[2].y = y - cosf(a + 0.55f) * w;
    lv_draw_triangle(layer, &t);
}

// Minimal display's marker: a notched arrowhead, white on black. A black
// copy a little larger goes underneath so it stays apart from the white route
// line it sits on.
static void chevron(lv_layer_t *layer, int32_t x, int32_t y, float a, float k, lv_color_t color)
{
    // (forward, right) in px: tip, right tail, notch, left tail.
    static const float pts[4][2] = {{17.0f, 0.0f}, {-12.0f, 12.0f}, {-6.0f, 0.0f}, {-12.0f, -12.0f}};
    lv_point_precise_t q[4];
    float sn = sinf(a), c = cosf(a);
    for (int i = 0; i < 4; i++) {
        float f = pts[i][0] * k, r = pts[i][1] * k;
        q[i].x = x + sn * f + c * r;
        q[i].y = y - c * f + sn * r;
    }
    lv_draw_triangle_dsc_t t;
    lv_draw_triangle_dsc_init(&t);
    t.color = color;
    t.opa = LV_OPA_COVER;
    t.p[0] = q[0], t.p[1] = q[1], t.p[2] = q[2];
    lv_draw_triangle(layer, &t);
    t.p[0] = q[0], t.p[1] = q[2], t.p[2] = q[3];
    lv_draw_triangle(layer, &t);
}

static void draw_chevron(lv_layer_t *layer, int32_t x, int32_t y, float a, bool live)
{
    chevron(layer, x, y, a, 1.45f, lv_color_black());
    chevron(layer, x, y, a, 1.0f, lv_color_hex(live ? 0xFFFFFF : COL_STALE));
}

static void draw_marker(lv_layer_t *layer, int32_t x, int32_t y, float rot)
{
    bool live = fix_is_live();
    // The marker sits on the route and, heading-up, its arrow lies along it.
    // Drawn in the route's own blue it vanished into the line, so on a light
    // map (route 0x1A73E8 in a white casing) the marker is a darker blue, and
    // the arrow is opaque with a white edge on both themes.
    if (s_minimal) {
        draw_chevron(layer, x, y, ((float)s_cog_cdeg / 100.0f - rot) * (float)M_PI / 180.0f, live);
        return;
    }
    lv_color_t c = lv_color_hex(!live ? COL_STALE : tiles_dark() ? COL_LIVE : COL_LIVE_L);
    int32_t hr = (int32_t)lroundf(MARK_HALO_R), dr = (int32_t)lroundf(MARK_DOT_R);

    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.radius = LV_RADIUS_CIRCLE;
    r.bg_color = c;
    r.bg_opa = LV_OPA_20;
    lv_area_t halo = {x - hr, y - hr, x + hr, y + hr};
    lv_draw_rect(layer, &r, &halo);

    if (live && s_moving) {
        float a = ((float)s_cog_cdeg / 100.0f - rot) * (float)M_PI / 180.0f;
        draw_arrow(layer, x, y, a, MARK_CONE_LEN + 2.0f * MARK_EDGE, MARK_CONE_W + MARK_EDGE,
                   lv_color_white());
        draw_arrow(layer, x, y, a, MARK_CONE_LEN, MARK_CONE_W, c);
    }

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
    r.border_color = lv_color_hex(dark_bg() ? 0x000000 : 0xFFFFFF);
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
#define LINK_BOW 0.22f     // perpendicular control offset, fraction of chord
#define LINK_SEG 96        // curve samples; fine enough that dots look round

// White on a dark map, light grey on a light one.
#define LINK_COL_DARK 0xFFFFFF
#define LINK_COL_LIGHT 0x8A9096

static void draw_dest_line(lv_layer_t *layer, int32_t x0, int32_t y0,
                           int32_t x1, int32_t y1, int32_t phase)
{
    float dx = (float)(x1 - x0), dy = (float)(y1 - y0);
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 4.0f) return;

    // Quadratic Bezier from us to the destination, with the control point
    // pushed perpendicular to the chord so the path bows to one side. The bow
    // scales with the chord length, so short and long hops look alike.
    float ux = dx / len, uy = dy / len;          // along the chord
    float px = -uy, py = ux;                     // perpendicular (screen space)
    float mx = (x0 + x1) * 0.5f, my = (y0 + y1) * 0.5f;
    float bow = len * LINK_BOW;                  // 0 = straight
    float cxp = mx + px * bow, cyp = my + py * bow;

    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LINK_W;              // capsule caps
    d.bg_color = lv_color_hex(dark_bg() ? LINK_COL_DARK : LINK_COL_LIGHT);
    d.bg_opa = LV_OPA_70;
    d.border_opa = LV_OPA_TRANSP;

    // Walk the curve by parameter t (not arclength): dashes stay roughly even
    // on gentle bows, which is all we need here. Sample in fine steps and paint
    // a dot wherever the running offset falls in a dash window.
    const int period = LINK_DASH + LINK_GAP;
    int32_t p = phase % period;
    float s_off = 0.0f;   // arclength travelled along the curve
    lv_point_precise_t prev = {x0, y0};
    for (int64_t i = 0; i <= LINK_SEG; i++) {
        float t = (float)i / (float)LINK_SEG;
        float omt = 1.0f - t;
        float bx = omt * omt * x0 + 2.0f * omt * t * cxp + t * t * x1;
        float by = omt * omt * y0 + 2.0f * omt * t * cyp + t * t * y1;
        if (i > 0) {
            // Offset along the curve, advanced by the segment length so the
            // dash phase marches consistently along the whole path.
            float seg = sqrtf((bx - prev.x) * (bx - prev.x) +
                              (by - prev.y) * (by - prev.y));
            float s = s_off;
            s_off += seg;
            // Dots are placed where (s + phase) lands in the dash window.
            float off = fmodf(s + (float)p, (float)period);
            if (off >= 0 && off < (float)LINK_DASH) {
                lv_area_t e = {
                    .x1 = (int32_t)lroundf(bx) - LINK_W / 2,
                    .y1 = (int32_t)lroundf(by) - LINK_W / 2,
                    .x2 = (int32_t)lroundf(bx) + LINK_W / 2,
                    .y2 = (int32_t)lroundf(by) + LINK_W / 2,
                };
                lv_draw_rect(layer, &d, &e);
            }
        }
        prev.x = bx;
        prev.y = by;
    }
}

// Route polyline. Drawn before the markers so they sit on top; the bowed
// "suggested route" line is suppressed when this is present. Screen-space
// projection is shared with the marker, so it follows pan/zoom/heading-up.
// Origin + rotation used by the frame being rendered (blit + marker agree).
static int32_t s_f_ox, s_f_oy;
static float s_f_rot;

#define ROUTE_CASING 9
#define ROUTE_W 5
#define ROUTE_W_MINI 11

// Trip progress along the bottom edge of the round screen (minimal display).
// Angles are LVGL's: clockwise from 3 o'clock, so 90 is the bottom. The arc
// fills from its left end; 50..130 keeps both ends clear of the turn banner.
#define PROG_A0 50
#define PROG_A1 130
#define PROG_R 226

static void draw_text(lv_layer_t *layer, const char *txt, const lv_font_t *font, lv_color_t col,
                      int32_t cx, int32_t cy, int32_t w)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = txt;
    d.text_local = 1;  // drawn later: LVGL must keep its own copy
    d.font = font;
    d.color = col;
    d.align = LV_TEXT_ALIGN_CENTER;
    int32_t h = lv_font_get_line_height(font);
    lv_area_t a = {cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2};
    lv_draw_label(layer, &d, &a);
}

// The posted limit as a road sign: white disc, red ring, black number.
// Nothing is drawn while the limit is unknown.
static void draw_limit_sign(lv_layer_t *layer, int32_t cx, int32_t cy, int32_t r,
                            const lv_font_t *font)
{
    uint16_t lim = nav_view_limit();
    if (!lim) return;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_color = lv_color_white();
    d.bg_opa = LV_OPA_COVER;
    d.border_color = lv_color_hex(0xD93025);
    d.border_width = r / 5 + 1;
    d.border_opa = LV_OPA_COVER;
    lv_area_t a = {cx - r, cy - r, cx + r, cy + r};
    lv_draw_rect(layer, &d, &a);
    char buf[8];
    lv_snprintf(buf, sizeof(buf), "%u", lim);
    draw_text(layer, buf, font, lv_color_black(), cx, cy, 2 * r);
}

// Speedometer. The scale runs 270 degrees clockwise from the lower left
// (LVGL angle 135) in SPD_SEGS steps. Steps up to the current speed are white
// bars that get longer as they go; the rest are dots. A red dot marks the limit.
#define SPD_A0 135.0f
#define SPD_SWEEP 270.0f
#define SPD_SEGS 54
#define SPD_R_OUT 228
#define SPD_R_DOT 210
#define SPD_MAX_KMH 160
#define SPD_MAX_MPH 100

static int speed_units(void)
{
    // cm/s -> km/h or mph, rounded.
    float k = nav_view_mph() ? 0.0223694f : 0.036f;
    return (int)lroundf(s_speed_cms * k);
}

static void draw_speedo(lv_layer_t *layer)
{
    bool live = fix_is_live();
    int max = nav_view_mph() ? SPD_MAX_MPH : SPD_MAX_KMH;
    int v = live ? speed_units() : 0;
    uint16_t lim = nav_view_limit();
    int filled = v * SPD_SEGS / max;
    if (filled > SPD_SEGS) filled = SPD_SEGS;

    lv_draw_line_dsc_t bar;
    lv_draw_line_dsc_init(&bar);
    bar.color = lv_color_white();
    bar.width = 7;
    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.radius = LV_RADIUS_CIRCLE;
    dot.bg_opa = LV_OPA_COVER;
    dot.bg_color = lv_color_hex(0x5F6368);
    for (int i = 0; i <= SPD_SEGS; i++) {
        float a = (SPD_A0 + SPD_SWEEP * i / SPD_SEGS) * (float)M_PI / 180.0f;
        float c = cosf(a), sn = sinf(a);
        if (i < filled) {
            float r_in = 204.0f - 26.0f * i / SPD_SEGS;
            bar.p1.x = HALF + c * r_in;
            bar.p1.y = HALF + sn * r_in;
            bar.p2.x = HALF + c * SPD_R_OUT;
            bar.p2.y = HALF + sn * SPD_R_OUT;
            lv_draw_line(layer, &bar);
        } else if (i % 3 == 0) {
            int32_t x = HALF + (int32_t)lroundf(c * SPD_R_DOT), y = HALF + (int32_t)lroundf(sn * SPD_R_DOT);
            lv_area_t d = {x - 2, y - 2, x + 2, y + 2};
            lv_draw_rect(layer, &dot, &d);
        }
    }
    if (lim && lim <= max) {
        float a = (SPD_A0 + SPD_SWEEP * lim / max) * (float)M_PI / 180.0f;
        int32_t x = HALF + (int32_t)lroundf(cosf(a) * SPD_R_DOT), y = HALF + (int32_t)lroundf(sinf(a) * SPD_R_DOT);
        dot.bg_color = lv_color_hex(0xD93025);
        lv_area_t d = {x - 6, y - 6, x + 6, y + 6};
        lv_draw_rect(layer, &dot, &d);
    }

    draw_limit_sign(layer, HALF, 150, 38, &lv_font_montserrat_28);

    char buf[24];
    if (live) lv_snprintf(buf, sizeof(buf), "%d", v);
    else lv_snprintf(buf, sizeof(buf), "--");
    bool over = live && lim && v > lim;
    draw_text(layer, buf, &lv_font_montserrat_48, over ? lv_color_hex(0xF28B82) : lv_color_white(),
              HALF, 236, 220);
    draw_text(layer, nav_view_mph() ? "mph" : "km/h", &lv_font_montserrat_20, lv_color_white(),
              HALF, 280, 120);

    // A pill like the reference's: the average speed while moving, once
    // there is half a minute of it to average; until then the compass heading.
    bool have_avg = s_trip_moving_ms >= 30000;
    if (have_avg || live) {
        if (have_avg) {
            float cms = (float)(s_trip_m * 100.0 / (s_trip_moving_ms / 1000.0));
            int tenths = (int)lroundf(cms * (nav_view_mph() ? 0.223694f : 0.36f));
            lv_snprintf(buf, sizeof(buf), "avg %d.%d", tenths / 10, tenths % 10);
        } else {
            static const char *const pts[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
            int deg = (s_cog_cdeg + 50) / 100 % 360;
            lv_snprintf(buf, sizeof(buf), "%s  %d\xC2\xB0", pts[(deg * 2 + 45) / 90 % 8], deg);
        }
        lv_draw_rect_dsc_t pill;
        lv_draw_rect_dsc_init(&pill);
        pill.radius = LV_RADIUS_CIRCLE;
        pill.bg_opa = LV_OPA_TRANSP;
        pill.border_color = lv_color_white();
        pill.border_width = 2;
        pill.border_opa = LV_OPA_COVER;
        lv_area_t a = {HALF - 95, 322, HALF + 95, 362};
        lv_draw_rect(layer, &pill, &a);
        draw_text(layer, buf, &lv_font_montserrat_20, lv_color_white(), HALF, 342, 180);
    }
}

// What the speedometer shows, folded into one number: a change means redraw.
static int32_t speedo_key(void)
{
    uint32_t k = (uint32_t)(fix_is_live() ? speed_units() + 1 : 0);  // 0..~250
    k = k * 257u + nav_view_limit();
    k = k * 2u + (nav_view_mph() ? 1u : 0u);
    // The average to a tenth, or the heading in whole degrees.
    uint32_t tail = s_trip_moving_ms >= 30000
                        ? (uint32_t)(s_trip_m * 1000.0 / (s_trip_moving_ms / 1000.0)) % 4096u
                        : 4096u + (s_cog_cdeg + 50) / 100 % 360;
    k = k * 8192u + tail;
    return (int32_t)(k & 0x7FFFFFFFu);
}

static void draw_progress(lv_layer_t *layer)
{
    int p = nav_view_progress_permille();
    s_prog_shown = p;
    if (p < 0) return;
    lv_draw_arc_dsc_t a;
    lv_draw_arc_dsc_init(&a);
    a.center.x = HALF;
    a.center.y = HALF;
    a.opa = LV_OPA_COVER;
    a.radius = PROG_R - 2;
    a.width = 3;
    a.color = lv_color_hex(0x5F6368);
    a.start_angle = PROG_A0;
    a.end_angle = PROG_A1;
    lv_draw_arc(layer, &a);
    if (p > 0) {
        a.radius = PROG_R;
        a.width = 7;
        a.rounded = 1;
        a.color = lv_color_white();
        a.start_angle = PROG_A1 - (PROG_A1 - PROG_A0) * p / 1000;
        lv_draw_arc(layer, &a);
    }
}

// Where the marker is along the route: segment index (points seg, seg + 1)
// and the fraction of that segment already covered. Found per frame in screen
// space, near the last answer first so a route that doubles back on itself
// cannot jump ahead to its return leg.
static int s_route_seg;
static float s_route_t;

// Nearest point on segments [lo, hi] to (mx, my). Returns the squared distance.
static float route_nearest(const int32_t *rx, const int32_t *ry, int lo, int hi, float mx, float my,
                           int *seg, float *frac)
{
    float best = 1e30f;
    for (int k = lo; k <= hi; k++) {
        float ax = rx[k], ay = ry[k];
        float dx = rx[k + 1] - ax, dy = ry[k + 1] - ay;
        float len2 = dx * dx + dy * dy;
        float t = len2 < 1e-3f ? 0.0f : ((mx - ax) * dx + (my - ay) * dy) / len2;
        t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
        float ex = ax + dx * t - mx, ey = ay + dy * t - my;
        float d2 = ex * ex + ey * ey;
        if (d2 < best) {
            best = d2;
            *seg = k;
            *frac = t;
        }
    }
    return best;
}

#define ROUTE_NEAR_PX 70.0f    // on the line: keep following it forward
#define ROUTE_LOST_PX 160.0f   // further than this from every segment: leave the split alone

static void route_locate(const int32_t *rx, const int32_t *ry)
{
    if (!s_has_fix) return;
    int32_t mx, my;
    marker_screen(s_f_ox, s_f_oy, s_f_rot, &mx, &my);
    int last = s_route_n - 2;
    int lo = s_route_seg - 2, hi = s_route_seg + 40;
    if (lo < 0) lo = 0;
    if (hi > last) hi = last;
    int seg = s_route_seg;
    float t = s_route_t;
    float d2 = route_nearest(rx, ry, lo, hi, mx, my, &seg, &t);
    if (d2 > ROUTE_NEAR_PX * ROUTE_NEAR_PX) {
        d2 = route_nearest(rx, ry, 0, last, mx, my, &seg, &t);
        if (d2 > ROUTE_LOST_PX * ROUTE_LOST_PX) return;
    }
    s_route_seg = seg;
    s_route_t = t;
}

static void route_seg(lv_layer_t *layer, lv_draw_line_dsc_t *d, float ax, float ay, float bx, float by)
{
    // Cull segments fully outside the (expanded) round viewport; a long route
    // is mostly off-screen at street zoom.
    float minx = ax < bx ? ax : bx, maxx = ax > bx ? ax : bx;
    float miny = ay < by ? ay : by, maxy = ay > by ? ay : by;
    if (maxx < -60 || minx > BOARD_LCD_W + 60 || maxy < -60 || miny > BOARD_LCD_H + 60) return;
    d->p1.x = ax;
    d->p1.y = ay;
    d->p2.x = bx;
    d->p2.y = by;
    lv_draw_line(layer, d);
}

static void draw_route(lv_layer_t *layer)
{
    if (!s_route_active || s_route_n < 2) return;

    static int32_t rx[ROUTE_MAX], ry[ROUTE_MAX];
    for (int i = 0; i < s_route_n; i++) {
        world_screen(lon_to_nx(s_route_lon[i] / 1e7), lat_to_ny(s_route_lat[i] / 1e7),
                     s_f_ox, s_f_oy, s_f_rot, &rx[i], &ry[i]);
    }
    if (s_route_seg > s_route_n - 2) s_route_seg = 0, s_route_t = 0.0f;
    route_locate(rx, ry);
    int seg = s_route_seg;
    // The point reached, where the line changes from covered to still ahead.
    float px = rx[seg] + (rx[seg + 1] - rx[seg]) * s_route_t;
    float py = ry[seg] + (ry[seg + 1] - ry[seg]) * s_route_t;

    bool dark = dark_bg();
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.round_start = 1;
    d.round_end = 1;
    // Two passes: a dark/white casing under a blue fill, so the line reads over
    // both road-coloured and water-coloured tiles. The minimal display has
    // nothing under the line, so it is one pass: a wide white stroke.
    // In each pass the part already covered goes down first, in grey, so the
    // part ahead overlaps it where they meet.
    for (int pass = s_minimal ? 1 : 0; pass < 2; pass++) {
        d.width = s_minimal ? ROUTE_W_MINI : pass == 0 ? ROUTE_CASING : ROUTE_W;
        d.opa = pass == 0 ? LV_OPA_70 : LV_OPA_COVER;
        lv_color_t casing = lv_color_hex(dark ? 0x0B0F14 : 0xFFFFFF);
        d.color = pass == 0 ? casing
                  : lv_color_hex(s_minimal ? COL_DONE_MINI : dark ? COL_DONE_D : COL_DONE_L);
        for (int k = 0; k < seg; k++) route_seg(layer, &d, rx[k], ry[k], rx[k + 1], ry[k + 1]);
        route_seg(layer, &d, rx[seg], ry[seg], px, py);

        d.color = pass == 0 ? casing
                  : s_minimal ? lv_color_white() : lv_color_hex(dark ? COL_ROUTE_D : COL_ROUTE_L);
        route_seg(layer, &d, px, py, rx[seg + 1], ry[seg + 1]);
        for (int k = seg + 1; k < s_route_n - 1; k++) {
            route_seg(layer, &d, rx[k], ry[k], rx[k + 1], ry[k + 1]);
        }
    }
}

typedef struct {
    int32_t tx, ty;
    int d;
} vis_tile_t;
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

// Black background for the minimal display. The frame buffers live in PSRAM,
// and so does the one the panel is scanning out: its interrupt copies that
// into a small internal buffer a few lines ahead of the beam. One 460 KB
// memset is the densest write PSRAM can be given, and it held that copy up
// long enough for the picture to tear and jump. So the clear goes in bands of
// about one of those copies each, with a pause after every band.
#define CLEAR_BAND_ROWS 16
#define CLEAR_GAP_US 250

static void clear_black(uint8_t *fb, uint32_t stride)
{
    for (int y = 0; y < BOARD_LCD_H; y++) {
        // Whole rows: skipping the corners left old pixels showing as a pale
        // rim, because things drawn near the edge reach past the disc the map
        // renderer treats as visible.
        memset(fb + (size_t)y * stride, 0, (size_t)BOARD_LCD_W * 2);
        if (y % CLEAR_BAND_ROWS == CLEAR_BAND_ROWS - 1) esp_rom_delay_us(CLEAR_GAP_US);
    }
}

// ---- nearby streets (minimal display) ----
// Points are kept as offsets from the set's origin in normalized world units:
// small numbers, so floats hold them exactly enough, where a float holding a
// whole-world coordinate would be a couple of pixels out at this zoom.
#define ROADS_MAX_PTS 1600
#define ROADS_MAX_LINES 240
// At full strength, beside the route. They fade to black away from it.
#define COL_ROAD_MINOR 0x4A5057
#define COL_ROAD_MAJOR 0x6E757E

typedef struct {
    uint16_t first, n;
    uint8_t cls;
} road_line_t;

typedef struct {
    double onx, ony;      // origin, normalized world
    float *dx, *dy;       // ROADS_MAX_PTS each
    road_line_t *lines;   // ROADS_MAX_LINES
    int n_pts, n_lines;
} road_set_t;

// Two sets: the one drawn, and the one being received.
static road_set_t s_roads[2];
static int s_roads_live;        // index of the set drawn
static bool s_roads_ok;         // buffers allocated
static bool s_roads_bad = true; // the set being received has a gap: ignore it
static uint8_t s_roads_seq;
static int32_t s_roads_olat, s_roads_olon;

static bool roads_alloc(void)
{
    if (s_roads_ok) return true;
    for (int i = 0; i < 2; i++) {
        s_roads[i].dx = malloc(ROADS_MAX_PTS * sizeof(float));
        s_roads[i].dy = malloc(ROADS_MAX_PTS * sizeof(float));
        s_roads[i].lines = malloc(ROADS_MAX_LINES * sizeof(road_line_t));
        if (!s_roads[i].dx || !s_roads[i].dy || !s_roads[i].lines) return false;
    }
    s_roads_ok = true;
    return true;
}

void map_view_roads_chunk(const uint8_t *data, size_t len)
{
    if (len < NAV_ROADS_HDR || data[0] != NAV_ROADS || !roads_alloc()) return;
    road_set_t *in = &s_roads[1 - s_roads_live];
    uint8_t flags = data[1];
    size_t o = NAV_ROADS_HDR;
    if (flags & NAV_ROADS_F_START) {
        if (len < NAV_ROADS_HDR + 8) return;
        memcpy(&s_roads_olat, data + 4, 4);
        memcpy(&s_roads_olon, data + 8, 4);
        o += 8;
        in->onx = lon_to_nx(s_roads_olon / 1e7);
        in->ony = lat_to_ny(s_roads_olat / 1e7);
        in->n_pts = in->n_lines = 0;
        s_roads_seq = 0;
        s_roads_bad = false;
    }
    if (s_roads_bad) return;
    if (data[2] != s_roads_seq) {
        s_roads_bad = true;
        return;
    }
    s_roads_seq++;
    while (o + 2 <= len) {
        int n = data[o], cls = data[o + 1];
        o += 2;
        if (n < 2 || o + (size_t)n * 4 > len) {
            s_roads_bad = true;
            return;
        }
        bool room = in->n_lines < ROADS_MAX_LINES && in->n_pts + n <= ROADS_MAX_PTS;
        if (room) in->lines[in->n_lines++] = (road_line_t){(uint16_t)in->n_pts, (uint16_t)n, (uint8_t)cls};
        for (int i = 0; i < n; i++, o += 4) {
            if (!room) continue;  // over budget: skip the line, keep parsing
            int16_t dlat, dlon;
            memcpy(&dlat, data + o, 2);
            memcpy(&dlon, data + o + 2, 2);
            in->dx[in->n_pts] = (float)(lon_to_nx((s_roads_olon + dlon * 100) / 1e7) - in->onx);
            in->dy[in->n_pts] = (float)(lat_to_ny((s_roads_olat + dlat * 100) / 1e7) - in->ony);
            in->n_pts++;
        }
    }
    if (flags & NAV_ROADS_F_END) {
        s_roads_live = 1 - s_roads_live;
        s_roads_bad = true;  // nothing more is expected until the next START
        invalidate();
    }
}

// ---- fading the streets away from the route ----
// A coarse map of how far each part of the screen is from the route ahead (or
// from the marker when there is no route): the screen in ROAD_CELL px cells,
// each holding a distance in half-cells. Streets are drawn at full strength
// next to the route and fade to nothing further out, so the route stays the
// only bright line and its surroundings read as context.
#define ROAD_CELL 8
#define ROAD_GRID (BOARD_LCD_W / ROAD_CELL)
#define ROAD_FADE_NEAR_PX 28    // full strength up to here
#define ROAD_FADE_FAR_PX 200    // invisible from here on
#define ROAD_SHADES 16
static uint8_t s_road_dist[ROAD_GRID * ROAD_GRID];
static uint8_t s_road_shade[256];  // distance value -> shade, 0 = not drawn

static inline void road_seed(int x, int y)
{
    if (x >= 0 && x < BOARD_LCD_W && y >= 0 && y < BOARD_LCD_H) {
        s_road_dist[(y / ROAD_CELL) * ROAD_GRID + x / ROAD_CELL] = 0;
    }
}

// World point -> screen for the frame being painted, same as draw_roads uses.
static inline void frame_screen(double nx, double ny, double w, float c, float sn, int *x, int *y)
{
    float wx = (float)((nx - s_cx) * w), wy = (float)((ny - s_cy) * w);
    *x = (int)lroundf(239.5f + wx * c + wy * sn);
    *y = (int)lroundf(239.5f - wx * sn + wy * c);
}

static void road_fade_build(double w, float c, float sn)
{
    memset(s_road_dist, 255, sizeof(s_road_dist));
    int x, y;
    if (s_has_fix) {
        frame_screen(s_mx, s_my, w, c, sn, &x, &y);
        road_seed(x, y);
    }
    if (s_route_active && s_route_n >= 2) {
        // The route still ahead, from the segment the marker is on.
        int from = s_route_seg > 0 && s_route_seg < s_route_n - 1 ? s_route_seg : 0;
        int px = 0, py = 0;
        for (int i = from; i < s_route_n; i++) {
            frame_screen(lon_to_nx(s_route_lon[i] / 1e7), lat_to_ny(s_route_lat[i] / 1e7), w, c, sn, &x, &y);
            if (i > from) {
                int minx = px < x ? px : x, maxx = px > x ? px : x;
                int miny = py < y ? py : y, maxy = py > y ? py : y;
                if (maxx >= 0 && minx < BOARD_LCD_W && maxy >= 0 && miny < BOARD_LCD_H &&
                    maxx - minx < 4000 && maxy - miny < 4000) {
                    int dx = x - px, dy = y - py;
                    int steps = (abs(dx) > abs(dy) ? abs(dx) : abs(dy)) / (ROAD_CELL / 2) + 1;
                    for (int k = 0; k <= steps; k++) road_seed(px + dx * k / steps, py + dy * k / steps);
                }
            }
            px = x, py = y;
        }
    }
    // Two-pass chamfer distance: 2 per cell straight, 3 diagonally.
    uint8_t *d = s_road_dist;
    for (int gy = 0; gy < ROAD_GRID; gy++) {
        for (int gx = 0; gx < ROAD_GRID; gx++) {
            int v = d[gy * ROAD_GRID + gx];
            if (gx > 0 && d[gy * ROAD_GRID + gx - 1] + 2 < v) v = d[gy * ROAD_GRID + gx - 1] + 2;
            if (gy > 0) {
                const uint8_t *u = d + (gy - 1) * ROAD_GRID;
                if (u[gx] + 2 < v) v = u[gx] + 2;
                if (gx > 0 && u[gx - 1] + 3 < v) v = u[gx - 1] + 3;
                if (gx < ROAD_GRID - 1 && u[gx + 1] + 3 < v) v = u[gx + 1] + 3;
            }
            d[gy * ROAD_GRID + gx] = (uint8_t)v;
        }
    }
    for (int gy = ROAD_GRID - 1; gy >= 0; gy--) {
        for (int gx = ROAD_GRID - 1; gx >= 0; gx--) {
            int v = d[gy * ROAD_GRID + gx];
            if (gx < ROAD_GRID - 1 && d[gy * ROAD_GRID + gx + 1] + 2 < v) v = d[gy * ROAD_GRID + gx + 1] + 2;
            if (gy < ROAD_GRID - 1) {
                const uint8_t *u = d + (gy + 1) * ROAD_GRID;
                if (u[gx] + 2 < v) v = u[gx] + 2;
                if (gx > 0 && u[gx - 1] + 3 < v) v = u[gx - 1] + 3;
                if (gx < ROAD_GRID - 1 && u[gx + 1] + 3 < v) v = u[gx + 1] + 3;
            }
            d[gy * ROAD_GRID + gx] = (uint8_t)v;
        }
    }
    // Distance value (half-cells) -> shade. Built once.
    static bool have_shade;
    if (!have_shade) {
        have_shade = true;
        for (int v = 0; v < 256; v++) {
            int px_d = v * (ROAD_CELL / 2);
            int sh = px_d <= ROAD_FADE_NEAR_PX ? ROAD_SHADES - 1
                     : px_d >= ROAD_FADE_FAR_PX ? 0
                     : (ROAD_FADE_FAR_PX - px_d) * (ROAD_SHADES - 1) / (ROAD_FADE_FAR_PX - ROAD_FADE_NEAR_PX);
            s_road_shade[v] = (uint8_t)sh;
        }
    }
}

// ROAD_SHADES steps of `hex` from black (never drawn) up to the colour itself.
static void road_ramp(uint32_t hex, uint16_t *out)
{
    int r = (hex >> 16) & 0xFF, g = (hex >> 8) & 0xFF, b = hex & 0xFF;
    for (int i = 0; i < ROAD_SHADES; i++) {
        int k = i * 255 / (ROAD_SHADES - 1);
        out[i] = lv_color_to_u16(lv_color_make(r * k / 255, g * k / 255, b * k / 255));
    }
}

// One thick line straight into the frame buffer, each step shaded by how far
// it is from the route. Not anti-aliased: these are dim background lines, and
// there can be a few hundred of them per frame, which LVGL's line drawing is
// far too slow for.
static void fb_line(uint8_t *fb, uint32_t stride, int x0, int y0, int x1, int y1,
                    const uint16_t *ramp, int w)
{
    int minx = x0 < x1 ? x0 : x1, maxx = x0 > x1 ? x0 : x1;
    int miny = y0 < y1 ? y0 : y1, maxy = y0 > y1 ? y0 : y1;
    if (maxx < 0 || minx >= BOARD_LCD_W || maxy < 0 || miny >= BOARD_LCD_H) return;
    // A segment that starts far off screen would walk a long way for nothing.
    if (maxx - minx > 4000 || maxy - miny > 4000) return;
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, h = w / 2;
    for (;;) {
        if (x0 >= h && x0 < BOARD_LCD_W - h && y0 >= h && y0 < BOARD_LCD_H - h) {
            int sh = s_road_shade[s_road_dist[(y0 / ROAD_CELL) * ROAD_GRID + x0 / ROAD_CELL]];
            if (sh) {
                uint16_t col = ramp[sh];
                for (int yy = y0 - h; yy <= y0 + h; yy++) {
                    uint16_t *p = (uint16_t *)(fb + (size_t)yy * stride) + (x0 - h);
                    for (int k = 0; k < w; k++) p[k] = col;
                }
            }
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) err += dy, x0 += sx;
        if (e2 <= dx) err += dx, y0 += sy;
    }
}

static void draw_roads(uint8_t *fb, uint32_t stride)
{
    if (!s_roads_ok) return;
    const road_set_t *rs = &s_roads[s_roads_live];
    if (!rs->n_lines) return;
    double w = world_px();
    float r = s_f_rot * (float)M_PI / 180.0f, c = cosf(r), sn = sinf(r), wf = (float)w;
    road_fade_build(w, c, sn);
    static uint16_t minor[ROAD_SHADES], major[ROAD_SHADES];
    static bool have_ramps;
    if (!have_ramps) {
        have_ramps = true;
        road_ramp(COL_ROAD_MINOR, minor);
        road_ramp(COL_ROAD_MAJOR, major);
    }
    // The origin's offset from the view centre, in pixels, before rotation.
    float bx = (float)((rs->onx - s_cx) * w), by = (float)((rs->ony - s_cy) * w);
    for (int l = 0; l < rs->n_lines; l++) {
        const road_line_t *ln = &rs->lines[l];
        int px = 0, py = 0;
        for (int i = 0; i < ln->n; i++) {
            float wx = bx + rs->dx[ln->first + i] * wf, wy = by + rs->dy[ln->first + i] * wf;
            int x = (int)lroundf(239.5f + wx * c + wy * sn), y = (int)lroundf(239.5f - wx * sn + wy * c);
            if (i) fb_line(fb, stride, px, py, x, y, ln->cls ? major : minor, ln->cls ? 5 : 3);
            px = x, py = y;
        }
    }
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
    if (s_minimal) {
        // No tiles: nothing is read from the SD card in this mode.
        clear_black(fb, stride);
        if (!s_speedo) draw_roads(fb, stride);
        return;
    }
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

// ---- the other faces ----

static bool face_ok(int f)
{
    return (f != FACE_ROUTE && f != FACE_ETA) || nav_view_active();
}

// The next face in direction dir (+1 / -1) that can show now, or -1.
static int face_step(int dir)
{
    for (int f = s_face + dir; f >= 0 && f < FACE_N; f += dir) {
        if (face_ok(f)) return f;
    }
    return -1;
}

// Local time now, seconds since the epoch, or 0 while the phone has not said.
static uint32_t clock_now(void)
{
    return s_clock_ok ? s_clock_epoch + (now_ms() - s_clock_at) / 1000 : 0;
}

static void fmt_clock(char *out, size_t cap, uint32_t local_s)
{
    uint32_t m = local_s / 60 % 1440;
    lv_snprintf(out, cap, "%02u:%02u", (unsigned)(m / 60), (unsigned)(m % 60));
}

// One dot per face that can show, the current one bright.
static void draw_dots(lv_layer_t *layer)
{
    int n = 0, at = 0;
    for (int f = 0; f < FACE_N; f++) {
        if (!face_ok(f)) continue;
        if (f == s_face) at = n;
        n++;
    }
    if (n < 2) return;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_opa = LV_OPA_COVER;
    for (int i = 0; i < n; i++) {
        int x = HALF + (2 * i - (n - 1)) * 8, r = i == at ? 4 : 3;
        d.bg_color = lv_color_hex(i == at ? 0xFFFFFF : 0x5F6368);
        lv_area_t a = {x - r, 438 - r, x + r, 438 + r};
        lv_draw_rect(layer, &d, &a);
    }
}

// Arrival: when, how long, how far, over the trip progress arc.
static void draw_eta(lv_layer_t *layer)
{
    char buf[32];
    int rem_s = nav_view_remaining_s();
    uint32_t now = clock_now();
    if (now) {
        char hm[8];
        fmt_clock(hm, sizeof(hm), now + (uint32_t)rem_s);
        lv_snprintf(buf, sizeof(buf), "ETA %s", hm);
        draw_text(layer, buf, &lv_font_montserrat_28, lv_color_hex(0xB0B8C0), HALF, 150, 300);
    }
    int mins = (rem_s + 30) / 60;
    if (mins >= 60) lv_snprintf(buf, sizeof(buf), "%dh %02dm", mins / 60, mins % 60);
    else lv_snprintf(buf, sizeof(buf), "%d min", mins);
    draw_text(layer, buf, &lv_font_montserrat_48, lv_color_white(), HALF, 214, 340);

    int rem = nav_view_remaining_m();
    if (rem < 0) rem = 0;
    if (nav_view_mph()) lv_snprintf(buf, sizeof(buf), "%d.%d mi", rem / 1609, (rem % 1609) * 10 / 1609);
    else lv_snprintf(buf, sizeof(buf), "%d.%d km", rem / 1000, (rem % 1000) / 100);
    lv_draw_rect_dsc_t box;
    lv_draw_rect_dsc_init(&box);
    box.radius = 14;
    box.bg_opa = LV_OPA_TRANSP;
    box.border_color = lv_color_hex(0x9AA0A6);
    box.border_width = 2;
    box.border_opa = LV_OPA_COVER;
    lv_area_t a = {HALF - 90, 278, HALF + 90, 334};
    lv_draw_rect(layer, &box, &a);
    draw_text(layer, buf, &lv_font_montserrat_28, lv_color_white(), HALF, 306, 170);
    draw_progress(layer);
}

// A small sun: disc and eight rays. Marks the brightness buttons.
static void draw_sun(lv_layer_t *layer, int32_t cx, int32_t cy, int32_t r)
{
    lv_draw_rect_dsc_t disc;
    lv_draw_rect_dsc_init(&disc);
    disc.radius = LV_RADIUS_CIRCLE;
    disc.bg_color = lv_color_white();
    disc.bg_opa = LV_OPA_COVER;
    lv_area_t a = {cx - r, cy - r, cx + r, cy + r};
    lv_draw_rect(layer, &disc, &a);
    lv_draw_line_dsc_t ray;
    lv_draw_line_dsc_init(&ray);
    ray.color = lv_color_white();
    ray.width = r >= 8 ? 3 : 2;
    ray.round_start = ray.round_end = 1;
    for (int i = 0; i < 8; i++) {
        float ang = i * (float)M_PI / 4.0f;
        ray.p1.x = cx + cosf(ang) * (r + 4);
        ray.p1.y = cy + sinf(ang) * (r + 4);
        ray.p2.x = cx + cosf(ang) * (r * 2 + 3);
        ray.p2.y = cy + sinf(ang) * (r * 2 + 3);
        lv_draw_line(layer, &ray);
    }
}

#define SUN_Y 222
#define SUN_DIM_X 66
#define SUN_BRIGHT_X 414

// The phone's battery as a ring, the time, and the brightness buttons: the
// small sun on the left dims, the large one on the right brightens.
static void draw_phone(lv_layer_t *layer)
{
    char buf[24];
    uint32_t now = clock_now();
    if (now) {
        fmt_clock(buf, sizeof(buf), now);
        draw_text(layer, buf, &lv_font_montserrat_28, lv_color_hex(0xB0B8C0), HALF, 96, 200);
    }

    const int32_t cy = 222, r = 82;
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.center.x = HALF;
    arc.center.y = cy;
    arc.radius = r;
    arc.width = 9;
    arc.opa = LV_OPA_COVER;
    arc.color = lv_color_hex(0x33383E);
    arc.start_angle = 0;
    arc.end_angle = 360;
    lv_draw_arc(layer, &arc);
    if (s_phone_batt > 0) {
        arc.rounded = 1;
        arc.color = lv_color_hex(s_phone_charging ? 0x34A853 : s_phone_batt <= 20 ? 0xD93025 : 0xFFFFFF);
        arc.start_angle = 270;
        arc.end_angle = 270 + 360 * s_phone_batt / 100;
        lv_draw_arc(layer, &arc);
    }
    // A phone outline in the ring, with the figure under it.
    lv_draw_rect_dsc_t ph;
    lv_draw_rect_dsc_init(&ph);
    ph.radius = 7;
    ph.bg_opa = LV_OPA_TRANSP;
    ph.border_color = lv_color_white();
    ph.border_width = 3;
    ph.border_opa = LV_OPA_COVER;
    lv_area_t body = {HALF - 16, cy - 46, HALF + 16, cy + 8};
    lv_draw_rect(layer, &ph, &body);
    if (s_phone_batt >= 0) lv_snprintf(buf, sizeof(buf), "%d%%", s_phone_batt);
    else lv_snprintf(buf, sizeof(buf), "--");
    draw_text(layer, buf, &lv_font_montserrat_28, lv_color_white(), HALF, cy + 36, 140);

    draw_sun(layer, SUN_DIM_X, SUN_Y, 5);
    draw_sun(layer, SUN_BRIGHT_X, SUN_Y, 9);
    if (s_bright_cb) {
        lv_snprintf(buf, sizeof(buf), "Brightness %d%%", s_bright_cb(0));
        draw_text(layer, buf, &lv_font_montserrat_20, lv_color_hex(0x9AA0A6), HALF, 356, 260);
    }
}

// What the current face shows, folded into one number: a change means redraw.
static int32_t face_key(void)
{
    uint32_t minute = clock_now() / 60 % 1440;
    switch (s_face) {
    case FACE_SPEED:
        return speedo_key();
    case FACE_ETA: {
        int rem = nav_view_remaining_m();
        return (int32_t)(((uint32_t)(nav_view_remaining_s() / 30) * 4096u + (uint32_t)((rem < 0 ? 0 : rem) / 100) % 4096u) * 1440u + minute) & 0x7FFFFFFF;
    }
    default:
        return (int32_t)((((uint32_t)(s_phone_batt + 1) * 2u + (s_phone_charging ? 1u : 0u)) * 128u +
                          (uint32_t)(s_bright_cb ? s_bright_cb(0) : 0)) * 1440u + minute);
    }
}

static void draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    int32_t sx = 0, sy = 0;
    bool have_sx = false;
    if (s_minimal && s_speedo) {
        s_speedo_key = face_key();
        if (s_face == FACE_ETA) draw_eta(layer);
        else if (s_face == FACE_PHONE) draw_phone(layer);
        else draw_speedo(layer);
        draw_dots(layer);
        s_r_valid = true;
        return;
    }
    if (s_has_fix) {
        marker_screen(s_f_ox, s_f_oy, s_f_rot, &sx, &sy);
        have_sx = true;
    }
    // Valhalla route geometry, when present: the real line, drawn first so
    // both markers sit on top. Otherwise a suggested dotted line from us to
    // the destination (the only thing possible before Valhalla, since OsmAnd
    // exposed no geometry). The phase advances one step per rendered frame
    // (not wall-clock): tying it to now_ms() made the pattern jitter.
    bool route_on = s_route_active && s_route_n >= 2;
    if (route_on) draw_route(layer);
    if (s_minimal) {
        // While a turn is showing, a translucent black band from just above it
        // to the bottom of the screen, over the route so the text reads
        // cleanly, and under the progress arc so that stays bright.
        if (nav_view_active()) {
            lv_draw_rect_dsc_t band;
            lv_draw_rect_dsc_init(&band);
            band.bg_color = lv_color_black();
            band.bg_opa = LV_OPA_70;
            lv_area_t a = {0, nav_view_banner_top() - 6, BOARD_LCD_W - 1, BOARD_LCD_H - 1};
            lv_draw_rect(layer, &band, &a);
        }
        draw_progress(layer);
        // Right of the arrow, above the turn banner.
        draw_limit_sign(layer, 392, 284, 32, &lv_font_montserrat_20);
    }
    if (s_has_dest && have_sx && !route_on) {
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
    if (s_zoom >= zoom_max()) {
        lv_obj_add_state(s_btn_in, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(s_btn_in, LV_STATE_DISABLED);
    }
    if (s_zoom <= zoom_min()) {
        lv_obj_add_state(s_btn_out, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(s_btn_out, LV_STATE_DISABLED);
    }
    // Recentre only matters while browsing; during navigation the banner owns
    // that area of the screen.
    // The minimal display has no buttons and cannot be dragged or zoomed. A
    // long press brings the map and its buttons back. The speedometer also
    // drops the status pill, which would sit on its scale.
    lv_obj_set_hidden(s_btn_center, s_minimal || s_follow || nav_view_active());
    lv_obj_set_hidden(s_pill, s_minimal && s_speedo);
    lv_obj_set_hidden(s_btn_in, s_minimal);
    lv_obj_set_hidden(s_btn_out, s_minimal);
    lv_obj_set_hidden(s_btn_media, s_minimal);
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
    if (z < zoom_min()) z = zoom_min();
    if (z > zoom_max()) z = zoom_max();
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
    if (!s_minimal) {
        // The saved zoom and the tile cache belong to the map.
        tiles_set_active_zoom(z);
        s_zoom_dirty = true;
        s_zoom_changed_ms = now_ms();
    }
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

static void set_face(int f)
{
    if (f == s_face) return;
    s_face = f;
    s_speedo = f != FACE_ROUTE;
    s_speedo_key = -1;
    nav_view_set_suppressed(s_speedo);
    update_buttons();
    invalidate();
}

static void map_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;

    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(indev, &s_last_pt);
        s_press_pt = s_last_pt;
        s_drag_acc = 0;
        s_dragging = s_pinching = s_pinch_done = false;
        s_press_t0 = now_ms();
        s_press_fired = false;
    } else if (s_minimal) {
        // No dragging and no zooming here. Three gestures only: hold still to
        // go back to the map, and swipe right or left to change face.
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        int32_t dx = p.x - s_press_pt.x, dy = p.y - s_press_pt.y;
        if (code == LV_EVENT_PRESSING) {
            if (!s_press_fired && s_press_t0 && abs(dx) + abs(dy) < DRAG_SLOP_PX * 2 &&
                touch_get()->n < 2 && now_ms() - s_press_t0 >= MODE_HOLD_MS) {
                s_press_fired = true;
                s_mode_req = true;
            }
        } else if (code == LV_EVENT_RELEASED && !s_press_fired) {
            if (abs(dx) >= SWIPE_MIN_PX && abs(dx) > 2 * abs(dy)) {
                // The faces sit in a row: music, route, speedometer, arrival,
                // phone. Swiping right moves along it and left moves back; left
                // from the first face that can show opens the music controls,
                // so music is always one swipe from the route.
                int f = face_step(dx > 0 ? 1 : -1);
                if (f >= 0) {
                    s_speedo_pref = f;
                    set_face(f);
                } else if (dx < 0 && s_media_cb) {
                    s_media_cb();  // only asks for the screen; main.c switches later
                }
            } else if (s_face == FACE_PHONE && s_speedo && s_bright_cb &&
                       abs(dx) + abs(dy) < DRAG_SLOP_PX * 2 && abs(p.y - SUN_Y) < 70) {
                // A tap on a sun: ten percent down on the left, up on the right.
                if (p.x < SUN_DIM_X + 60) s_bright_cb(-10);
                else if (p.x > SUN_BRIGHT_X - 60) s_bright_cb(10);
            }
        }
        return;
    } else if (code == LV_EVENT_PRESSING) {
        const touch_sample_t *t = touch_get();
        // A finger held still switches between the map and the minimal display.
        if (t->n < 2 && !s_dragging && !s_pinching && !s_press_fired && s_press_t0 &&
            now_ms() - s_press_t0 >= MODE_HOLD_MS) {
            s_press_fired = true;
            s_mode_req = true;
        }
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
    s_btn_media = ui_round_button(parent, LV_SYMBOL_AUDIO, 70, 240, 58, f, media_btn_cb);

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

    if (!tiles_zoom_min() && !s_minimal) {
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
    s_btn_in = s_btn_out = s_btn_center = s_btn_media = s_toast = NULL;
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
    // Trip counter: distance as speed times the time since the last fix, only
    // while moving and only across a normal gap (not a link dropout).
    if (s_fix_live && speed_cms >= MOVING_OFF_CMS) {
        uint32_t gap = t - s_last_fix_ms;
        if (gap > 0 && gap < 3000) {
            s_trip_m += speed_cms / 100.0 * gap / 1000.0;
            s_trip_moving_ms += gap;
        }
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

// ---- route line ----------------------------------------------------------

void map_view_route_reset(void)
{
    s_route_n = 0;
    s_route_active = false;
}

void map_view_route_add(const int32_t *lat_e7, const int32_t *lon_e7, int n)
{
    for (int i = 0; i < n && s_route_n < ROUTE_MAX; i++) {
        s_route_lat[s_route_n] = lat_e7[i];
        s_route_lon[s_route_n] = lon_e7[i];
        s_route_n++;
    }
}

void map_view_route_commit(void)
{
    s_route_active = s_route_n >= 2;
    s_route_seg = 0;  // a new line: start looking from its beginning
    s_route_t = 0.0f;
    invalidate();
}

void map_view_route_clear(void)
{
    if (!s_route_active && s_route_n == 0) return;
    s_route_n = 0;
    s_route_active = false;
    invalidate();
}

bool map_view_route_active(void)
{
    return s_route_active && s_route_n >= 2;
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

void map_view_set_phone(int battery_pct, bool charging, uint32_t local_epoch_s)
{
    s_phone_batt = battery_pct;
    s_phone_charging = charging;
    s_clock_epoch = local_epoch_s;
    s_clock_at = now_ms();
    s_clock_ok = true;
}

void map_view_set_brightness_cb(int (*cb)(int delta))
{
    s_bright_cb = cb;
}

void map_view_set_link(bool connected, bool paired)
{
    s_connected = connected;
    s_paired = paired;
    if (!connected) s_phone_batt = -1;  // unknown again; the clock keeps running
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
    if (s_minimal) {
        // Pull back as a turn comes up, and close in again once it is behind.
        int d = nav_view_dist_m();
        if (d < 0 || d > TURN_FAR_M) s_turn_near = false;
        else if (d <= TURN_NEAR_M) s_turn_near = true;
        double target = s_turn_near ? TURN_SCALE : CRUISE_SCALE;
        float dts = (t - s_scale_t) / 1000.0f;
        s_scale_t = t;
        if (dts > 0.1f) dts = 0.1f;
        if (fabs(target - s_scale) < 0.002) s_scale = target;
        else s_scale += (target - s_scale) * (1.0 - exp(-dts * SCALE_RATE));
    }
    if (s_follow && s_has_fix) {
        s_cx = s_mx;
        s_cy = s_my;
        if (s_minimal) {
            // Look ahead: put the view centre MINI_AHEAD_PX up the screen from
            // the marker, so the marker draws that far below the centre.
            double w = world_px();
            float r = s_rot * (float)M_PI / 180.0f;
            s_cx = clamp01(s_mx + MINI_AHEAD_PX * sinf(r) / w);
            s_cy = clamp01(s_my - MINI_AHEAD_PX * cosf(r) / w);
        }
    }

    // Heading-up only while following a moving, live fix; otherwise ease
    // back to north-up (which also re-enables the fast unrotated blit). The
    // minimal display stays heading-up while stopped: it has no tiles to blit,
    // and a route that swings round at every red light is hard to read.
    {
        bool heading_up = s_follow && s_has_fix && fix_is_live() && (s_moving || s_minimal);
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
        s_speedo_pref = -1;  // a drive started or ended: back to the automatic face
        update_buttons();
    }
    if (s_minimal) {
        set_face(s_speedo_pref >= 0 && face_ok(s_speedo_pref) ? s_speedo_pref
                 : nav_on ? FACE_ROUTE : FACE_SPEED);
    }
    nav_view_tick();  // staleness must hide the banner even without a stop message
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
    if (s_mode_req) {
        s_mode_req = false;
        if (s_mode_cb) s_mode_cb();
    }
    if (s_minimal && s_speedo) {
        if (face_key() != s_speedo_key) lv_obj_invalidate(s_map);
    } else if (s_minimal && nav_view_progress_permille() != s_prog_shown) {
        lv_obj_invalidate(s_map);
    }
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

    if (s_minimal && s_speedo) {
        // The speedometer redraws only when a figure on it changes (the key
        // check above). The map's own triggers below follow the view origin,
        // the marker and the destination line, none of which it draws: left
        // on, they repainted the whole screen on every pass while moving,
        // faster than the panel swaps buffers, which showed as flicker.
    } else if (loaded) {
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

    if (!s_minimal && t - s_prefetch_ms > PREFETCH_MS) {
        s_prefetch_ms = t;
        prefetch_ring();
    }
    if (s_toast_until && (int32_t)(t - s_toast_until) > 0) {
        s_toast_until = 0;
        lv_obj_set_hidden(s_toast, true);
    }
}

void map_view_set_minimal(bool on)
{
    if (on == s_minimal) return;
    // Each mode keeps its own zoom; swap them.
    int z = s_zoom;
    s_zoom = s_zoom_other;
    s_zoom_other = z;
    s_minimal = on;
    if (!on) tiles_set_active_zoom(s_zoom);
    s_prog_shown = -2;
    s_scale = on ? CRUISE_SCALE : 1.0;
    s_turn_near = false;
    s_follow = true;  // nothing can drag the minimal display off the marker
    s_face = FACE_ROUTE;
    s_speedo = false;
    s_speedo_pref = -1;
    nav_view_set_suppressed(false);
    map_view_recenter();
    update_buttons();
    invalidate();
}

void map_view_set_mode_cb(void (*cb)(void))
{
    s_mode_cb = cb;
}
