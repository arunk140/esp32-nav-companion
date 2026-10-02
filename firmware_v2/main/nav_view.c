#include "nav_view.h"
#include "board.h"
#include "display.h"
#include "ui_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// Bottom-centred banner. On a round panel the usable width shrinks towards
// the bottom: at y=410 the visible chord is ~339 px, so 340 px with the bottom
// edge resting there has its corners just meet the bezel instead of being
// clipped (the guide's geometry table).
#define BANNER_W 340
#define BANNER_H 84
#define BANNER_RADIUS 16
#define BANNER_X ((BOARD_LCD_W - BANNER_W) / 2)
#define BANNER_BOTTOM 410
#define BANNER_Y (BANNER_BOTTOM - BANNER_H)
#define ICON_CX 44           // banner-local centre of the maneuver icon
#define ICON_CY (BANNER_H / 2)
#define ICON_D 64
#define TEXT_X 88            // banner-local left edge of the text column
#define TEXT_W (BANNER_W - TEXT_X - 12)
#define STALE_MS 6000         // no update for this long -> treat as ended

#define COL_BG 0x1B2027
#define COL_ICON 0xF1F3F4
#define COL_STREET 0xFFFFFF
#define COL_DIST 0xA8B0B8
#define COL_WARN 0xF28B82
#define MAX_DIST_LABEL 8

// ---- persistent state (survives map screen rebuilds) ----
static nav_state_t s_st;
static char s_street[NAV_TEXT_MAX + 1];
static bool s_have;
static uint32_t s_last_ms;

// ---- UI ----
static lv_obj_t *s_banner, *s_lbl_street, *s_lbl_dist, *s_lbl_left;
static void (*s_click_cb)(void);

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// "450 m", "1.2 km" - compact, so it fits next to the street name.
static void format_dist(char *out, size_t cap, uint16_t m)
{
    if (m >= 1000) snprintf(out, cap, "%u.%u km", m / 1000, (m % 1000) / 100);
    else snprintf(out, cap, "%u m", m);
}

// ---------------------------------------------------------------- icons
// Authored in a 100x100 box centred on (cx, cy), then scaled by ICON_PCT so a
// single knob shrinks every icon without touching the text.
#define ICON_PCT 90
static int ip(int v) { return (v * ICON_PCT) / 100; }

// A stroke is a thick line with rounded caps; heads are triangles. Lines must
// use lv_draw_line (true rotated capsules) - axis-aligned rects only work for
// horizontal/vertical strokes and turn into blobs on every diagonal.
typedef struct {
    lv_color_t color;
    int w;
} icon_pen_t;

static void pen_init(icon_pen_t *p, lv_color_t col, int w)
{
    p->color = col;
    p->w = w;
}

// Straight segment from (x1,y1) to (x2,y2) with rounded caps.
static void seg(lv_layer_t *l, const icon_pen_t *p, int x1, int y1, int x2, int y2)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = p->color;
    d.opa = LV_OPA_COVER;
    d.width = p->w;
    d.round_start = 1;
    d.round_end = 1;
    d.p1.x = x1; d.p1.y = y1;
    d.p2.x = x2; d.p2.y = y2;
    lv_draw_line(l, &d);
}

static void arrow_head(lv_layer_t *l, const icon_pen_t *p, int x, int y, float dir_deg)
{
    // Triangle pointing along dir_deg (0 = up, 90 = right).
    float a = dir_deg * (float)M_PI / 180.0f;
    float s = sinf(a), c = cosf(a);
    const int tip = ip(22), back = ip(12), wid = ip(13);
    lv_point_precise_t v[3];
    v[0].x = x + s * tip;      v[0].y = y - c * tip;
    v[1].x = x - s * back + c * wid; v[1].y = y + c * back + s * wid;
    v[2].x = x - s * back - c * wid; v[2].y = y + c * back - s * wid;
    lv_draw_triangle_dsc_t t;
    lv_draw_triangle_dsc_init(&t);
    t.color = p->color;
    t.opa = LV_OPA_COVER;
    t.p[0] = v[0]; t.p[1] = v[1]; t.p[2] = v[2];
    lv_draw_triangle(l, &t);
}

// shaft + head, the base of every straight-ahead icon
static void straight(lv_layer_t *l, const icon_pen_t *p, int cx, int cy)
{
    seg(l, p, cx, cy + ip(34), cx, cy - ip(12));
    arrow_head(l, p, cx, cy - ip(14), 0.0f);
}

static void draw_icon(lv_layer_t *l, int cx, int cy, uint8_t type)
{
    icon_pen_t p;
    bool warn = type == NAV_OFF_ROUTE;
    pen_init(&p, lv_color_hex(warn ? COL_WARN : COL_ICON), ip(13));
    int w = p.w;

    switch (type) {
    case NAV_CONTINUE:
        straight(l, &p, cx, cy);
        break;
    case NAV_SLIGHT_LEFT:
        seg(l, &p, cx, cy + ip(36), cx, cy - ip(6));
        seg(l, &p, cx, cy - ip(6), cx - ip(16), cy - ip(22));
        arrow_head(l, &p, cx - ip(18), cy - ip(24), -40.0f);
        break;
    case NAV_LEFT:
        seg(l, &p, cx, cy + ip(36), cx, cy - ip(8));
        seg(l, &p, cx, cy - ip(8), cx - ip(18), cy - ip(8));
        arrow_head(l, &p, cx - ip(20), cy - ip(8), -90.0f);
        break;
    case NAV_SHARP_LEFT:
        seg(l, &p, cx, cy + ip(36), cx, cy - ip(6));
        seg(l, &p, cx, cy - ip(6), cx - ip(20), cy + ip(10));
        arrow_head(l, &p, cx - ip(22), cy + ip(12), -130.0f);
        break;
    case NAV_SLIGHT_RIGHT:
        seg(l, &p, cx, cy + ip(36), cx, cy - ip(6));
        seg(l, &p, cx, cy - ip(6), cx + ip(16), cy - ip(22));
        arrow_head(l, &p, cx + ip(18), cy - ip(24), 40.0f);
        break;
    case NAV_RIGHT:
        seg(l, &p, cx, cy + ip(36), cx, cy - ip(8));
        seg(l, &p, cx, cy - ip(8), cx + ip(18), cy - ip(8));
        arrow_head(l, &p, cx + ip(20), cy - ip(8), 90.0f);
        break;
    case NAV_SHARP_RIGHT:
        seg(l, &p, cx, cy + ip(36), cx, cy - ip(6));
        seg(l, &p, cx, cy - ip(6), cx + ip(20), cy + ip(10));
        arrow_head(l, &p, cx + ip(22), cy + ip(12), 130.0f);
        break;
    case NAV_KEEP_LEFT:
        seg(l, &p, cx + ip(10), cy + ip(36), cx + ip(10), cy - ip(6));
        seg(l, &p, cx + ip(10), cy - ip(6), cx - ip(18), cy - ip(6));
        arrow_head(l, &p, cx - ip(20), cy - ip(6), -90.0f);
        break;
    case NAV_KEEP_RIGHT:
        seg(l, &p, cx - ip(10), cy + ip(36), cx - ip(10), cy - ip(6));
        seg(l, &p, cx - ip(10), cy - ip(6), cx + ip(18), cy - ip(6));
        arrow_head(l, &p, cx + ip(20), cy - ip(6), 90.0f);
        break;
    case NAV_UTURN: {
        // Up, back down, then a downward head: a hairpin.
        seg(l, &p, cx + ip(14), cy + ip(36), cx + ip(14), cy - ip(14));
        seg(l, &p, cx + ip(14), cy - ip(14), cx - ip(10), cy - ip(14));
        seg(l, &p, cx - ip(10), cy - ip(14), cx - ip(10), cy + ip(8));
        arrow_head(l, &p, cx - ip(10), cy + ip(10), 180.0f);
        break;
    }
    case NAV_ROUNDABOUT: {
        // A ring with a gap on the right, and an exit arrow out of the top right.
        lv_draw_arc_dsc_t a;
        lv_draw_arc_dsc_init(&a);
        a.color = p.color;
        a.opa = LV_OPA_COVER;
        a.width = p.w;
        a.center.x = cx;
        a.center.y = cy;
        a.radius = ip(22);
        a.start_angle = 0;
        a.end_angle = 270;
        lv_draw_arc(l, &a);
        seg(l, &p, cx + ip(12), cy - ip(12), cx + ip(26), cy - ip(22));
        arrow_head(l, &p, cx + ip(27), cy - ip(23), 40.0f);
        break;
    }
    case NAV_OFF_ROUTE: {
        // Exclamation: this is the one icon that must read as "stop".
        seg(l, &p, cx, cy - ip(28), cx, cy + ip(2));
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = p.color;
        d.bg_opa = LV_OPA_COVER;
        d.radius = LV_RADIUS_CIRCLE;
        lv_area_t dot = {cx - w, cy + ip(14), cx + w, cy + ip(14) + w * 2};
        lv_draw_rect(l, &d, &dot);
        break;
    }
    case NAV_ARRIVE:
    case NAV_NONE:
    default: {
        // Arrival: a filled disc, so "you are here" can't be mistaken for a turn.
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = p.color;
        d.bg_opa = LV_OPA_COVER;
        d.radius = LV_RADIUS_CIRCLE;
        int r = ip(26);
        lv_area_t a = {cx - r, cy - r, cx + r, cy + r};
        lv_draw_rect(l, &d, &a);
        break;
    }
    }
}

// ---------------------------------------------------------------- banner

static void refresh(void)
{
    if (!s_banner) return;
    bool active = s_have && (s_st.flags & NAV_F_ACTIVE) &&
                  (now_ms() - s_last_ms) < STALE_MS;
    lv_obj_set_hidden(s_banner, !active);
    if (!active) return;

    char buf[MAX_DIST_LABEL + 4];
    if (s_st.maneuver == NAV_ARRIVE) {
        snprintf(buf, sizeof(buf), "Arrived");
    } else if (s_st.maneuver == NAV_OFF_ROUTE) {
        snprintf(buf, sizeof(buf), "Off route");
    } else {
        format_dist(buf, sizeof(buf), s_st.dist_m);
    }
    lv_label_set_text(s_lbl_dist, buf);

    uint16_t rm = s_st.remaining_m;
    char left[24];
    if (rm >= 10000) snprintf(left, sizeof(left), "%.1f km left", rm / 1000.0f);
    else snprintf(left, sizeof(left), "%u m left", rm);
    lv_label_set_text(s_lbl_left, left);
}

static void banner_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *obj = lv_event_get_target(e);
    // Draw events use ABSOLUTE (screen) coordinates: the layer's clip area is
    // the object's absolute coords (see map_view's marker). Drawing at the
    // banner-local icon point falls outside that clip and vanishes, so offset
    // by the banner's own position.
    lv_area_t c;
    lv_obj_get_coords(obj, &c);
    draw_icon(layer, c.x1 + ICON_CX, c.y1 + ICON_CY, s_st.maneuver);
}

static void banner_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_click_cb) s_click_cb();
}

void nav_view_set_click_cb(void (*cb)(void))
{
    s_click_cb = cb;
}

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t col, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, TEXT_W);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, TEXT_X, y);  // banner-local
    return l;
}

void nav_view_init(void)
{
    lv_obj_t *scr = lv_screen_active();
    s_banner = lv_obj_create(scr);
    lv_obj_remove_style_all(s_banner);
    lv_obj_set_size(s_banner, BANNER_W, BANNER_H);
    lv_obj_set_pos(s_banner, BANNER_X, BANNER_Y);
    lv_obj_set_style_bg_color(s_banner, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_banner, BANNER_RADIUS, 0);
    lv_obj_set_clickable(s_banner, true);
    lv_obj_add_event_cb(s_banner, banner_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_banner, banner_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    s_lbl_street = mk_label(s_banner, &lv_font_montserrat_20, COL_STREET, 10);
    s_lbl_dist = mk_label(s_banner, &lv_font_montserrat_28, COL_ICON, 40);
    s_lbl_left = mk_label(s_banner, &lv_font_montserrat_14, COL_DIST, 52);
    lv_label_set_text(s_lbl_left, "");
    // The box is right-aligned, but the text inside defaults to left-aligned
    // and would draw at the same x as the big distance, overlapping it.
    lv_obj_set_style_text_align(s_lbl_left, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_lbl_left, LV_ALIGN_TOP_RIGHT, -12, 52);  // banner-local

    refresh();
}

void nav_view_destroy(void)
{
    s_banner = s_lbl_street = s_lbl_dist = s_lbl_left = NULL;
}

void nav_view_update(const nav_state_t *st, const char *street)
{
    // STATE and TEXT are separate writes, so either half may be absent: main.c
    // passes NULL for the one it is not updating.
    if (!st && !street) return;

    if (st) {
#if CONFIG_NAV_DEBUG_BLE
        static bool logged = false;
        if (!logged) {
            logged = true;
            ESP_LOGW("nav", "first state: active=%d maneuver=%u dist=%u after=%u+%u rem=%um/%us",
                     st->flags & NAV_F_ACTIVE, st->maneuver, st->dist_m, st->after_maneuver,
                     st->after_dist_m, st->remaining_m, st->remaining_s);
        }
#endif
        s_st = *st;
        s_have = true;
        s_last_ms = now_ms();
        // Changing the maneuver must repaint it: lv_label_set_text below only
        // invalidates the label areas, so the icon would otherwise stay stale.
        if (s_banner) lv_obj_invalidate(s_banner);
    }
    if (street) {
        // Same Latin-only rule as the media title: drop what we cannot draw.
        size_t o = 0;
        for (const unsigned char *p = (const unsigned char *)street;
             *p && o < NAV_TEXT_MAX; p++) {
            if (*p >= 0x80 && *p < 0xA0) continue;
            if (*p < 0x20 || *p == 0x7F) {
                if (o) s_street[o++] = ' ';
                continue;
            }
            s_street[o++] = (char)*p;
        }
        while (o > 0 && s_street[o - 1] == ' ') o--;
        s_street[o] = '\0';
        if (!o) snprintf(s_street, sizeof(s_street), "unnamed road");
    }
    if (s_lbl_street) {
        lv_label_set_text(s_lbl_street, s_street);
        refresh();
    }
}

bool nav_view_active(void)
{
    return s_have && (s_st.flags & NAV_F_ACTIVE) && (now_ms() - s_last_ms) < STALE_MS;
}

void nav_view_clear(void)
{
    s_have = false;
    s_street[0] = '\0';
    if (s_banner) {
        if (s_lbl_street) lv_label_set_text(s_lbl_street, "");
        lv_obj_set_hidden(s_banner, true);
    }
}
