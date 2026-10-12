#include "boot_view.h"
#include "board.h"
#include "esp_timer.h"
#include <math.h>
#include <stdio.h>

#define CX (BOARD_LCD_W / 2)
#define CY (BOARD_LCD_H / 2)
#define COL_ACCENT 0x4FC3F7   // the one colour: the two sweeping arcs
#define COL_DIM 0x7A8088
#define FRAME_MS 16

// The timeline, in ms at normal speed. A wake plays it QUICK times faster.
#define T_RING 620      // the rim ring and its ticks are complete
#define T_SWEEP_IN 280  // the inner arcs start to appear
#define T_ARROW 640     // the arrow starts to grow
#define T_ARROW_END 1120
#define T_NAME 1020     // the name starts to fade in
#define T_OUT 1650      // everything starts to fade
#define T_END 1950
#define QUICK 1.7f

static lv_obj_t *s_obj;
static lv_timer_t *s_timer;
static int64_t s_t0;
static float s_speed = 1.0f;
static char s_name[24];
static void (*s_on_done)(void);
static bool s_done;
static bool s_started;
static bool s_sleeping;  // run backwards, over a dimming copy of the screen

// Going to sleep starts from the finished picture and runs back to nothing.
#define T_SLEEP_FROM 1500
#define SLEEP_SPEED 1.3f
#define SLEEP_DIM_MS 240

static inline float clamp01(float v)
{
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

// 0 before a, 1 after b, a straight ramp between.
static inline float ramp(float t, float a, float b)
{
    return clamp01((t - a) / (b - a));
}

static inline float ease_out(float x)
{
    float y = 1.0f - x;
    return 1.0f - y * y * y;
}

// Overshoots a little past 1 and settles: the arrow lands, it does not just stop.
static inline float ease_back(float x)
{
    const float c = 1.70158f;
    float y = x - 1.0f;
    return 1.0f + (c + 1.0f) * y * y * y + c * y * y;
}

static void arc(lv_layer_t *l, int r, int w, float a0, float a1, uint32_t col, float opa)
{
    if (opa <= 0.01f || a1 - a0 < 0.5f) return;
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.center.x = CX;
    d.center.y = CY;
    d.radius = r;
    d.width = w;
    d.rounded = 1;
    d.color = lv_color_hex(col);
    d.opa = (lv_opa_t)(255.0f * clamp01(opa));
    d.start_angle = a0;
    d.end_angle = a1;
    lv_draw_arc(l, &d);
}

static void draw_cb(lv_event_t *e)
{
    lv_layer_t *l = lv_event_get_layer(e);
    // The clock starts with the first frame drawn: the view is built before
    // the GUI task runs, and the backlight comes on with that first frame.
    if (!s_started) {
        s_started = true;
        s_t0 = esp_timer_get_time();
    }
    float t = (float)((esp_timer_get_time() - s_t0) / 1000) * s_speed;
    float out = 1.0f - ramp(t, T_OUT, T_END);  // everything fades together at the end
    if (s_sleeping) {
        // Black comes down over the screen first, and the picture comes up
        // with it; then time runs backwards from the finished picture.
        float elapsed = (float)((esp_timer_get_time() - s_t0) / 1000);
        float dim = ramp(elapsed, 0, SLEEP_DIM_MS);
        lv_draw_rect_dsc_t bg;
        lv_draw_rect_dsc_init(&bg);
        bg.bg_color = lv_color_black();
        bg.bg_opa = (lv_opa_t)(255.0f * dim);
        lv_area_t all = {0, 0, BOARD_LCD_W - 1, BOARD_LCD_H - 1};
        lv_draw_rect(l, &bg, &all);
        t = T_SLEEP_FROM - elapsed * SLEEP_SPEED;
        if (t < 0.0f) return;  // nothing left: the screen stays black
        out = dim;
    }

    // The rim: a ring growing both ways from the top, its ticks appearing behind it.
    float grown = ease_out(ramp(t, 0, T_RING)) * 180.0f;
    arc(l, UI_SC(230), UI_SC(2), 270.0f - grown, 270.0f + grown, 0xFFFFFF, 0.55f * out);
    lv_draw_line_dsc_t tick;
    lv_draw_line_dsc_init(&tick);
    tick.width = UI_SC(2);
    tick.color = lv_color_white();
    for (int i = 0; i < 60; i++) {
        float off = i * 6.0f;                  // degrees clockwise from the top
        float dist = off > 180.0f ? 360.0f - off : off;
        if (dist > grown) continue;
        bool major = i % 5 == 0;
        // A tick fades in over the 25 degrees after the ring passes it.
        float o = clamp01((grown - dist) / 25.0f) * (major ? 0.9f : 0.35f) * out;
        if (o <= 0.01f) continue;
        float a = (off - 90.0f) * (float)M_PI / 180.0f, c = cosf(a), sn = sinf(a);
        int r0 = major ? UI_SC(206) : UI_SC(213);
        tick.opa = (lv_opa_t)(255.0f * o);
        tick.p1.x = CX + c * r0;
        tick.p1.y = CY + sn * r0;
        tick.p2.x = CX + c * UI_SC(222);
        tick.p2.y = CY + sn * UI_SC(222);
        lv_draw_line(l, &tick);
    }

    // Two arcs sweeping in opposite directions, one colour, gone before the end.
    float sweep = ramp(t, T_SWEEP_IN, T_SWEEP_IN + 320) * (1.0f - ramp(t, T_OUT - 420, T_OUT - 60)) * out;
    float turn = t * 0.30f;                   // degrees
    float a = fmodf(turn, 360.0f);
    arc(l, UI_SC(158), UI_SC(3), a, a + 80.0f, COL_ACCENT, 0.9f * sweep);
    arc(l, UI_SC(158), UI_SC(3), a + 180.0f, a + 215.0f, COL_ACCENT, 0.45f * sweep);
    float b = 360.0f - fmodf(turn * 1.6f, 360.0f);
    arc(l, UI_SC(138), UI_SC(2), b, b + 48.0f, COL_ACCENT, 0.6f * sweep);

    // The heading arrow, the one the map uses, growing into the middle. Its
    // base polygon is design-space too, so the growth factor carries the scale.
    float k = ramp(t, T_ARROW, T_ARROW_END);
    if (k > 0.0f) {
        float sc = ease_back(k) * 3.4f * UI_SCF(1.0f);
        static const float pts[4][2] = {{0, -17}, {12, 12}, {0, 6}, {-12, 12}};
        lv_draw_triangle_dsc_t tri;
        lv_draw_triangle_dsc_init(&tri);
        tri.color = lv_color_white();
        tri.opa = (lv_opa_t)(255.0f * clamp01(k * 3.0f) * out);
        lv_point_precise_t q[4];
        for (int i = 0; i < 4; i++) {
            q[i].x = CX + pts[i][0] * sc;
            q[i].y = CY - UI_SC(6) + pts[i][1] * sc;
        }
        tri.p[0] = q[0], tri.p[1] = q[1], tri.p[2] = q[2];
        lv_draw_triangle(l, &tri);
        tri.p[0] = q[0], tri.p[1] = q[2], tri.p[2] = q[3];
        lv_draw_triangle(l, &tri);
    }

    // The device's name under it.
    float name = ramp(t, T_NAME, T_NAME + 300) * out;
    if (name > 0.01f && s_name[0]) {
        lv_draw_label_dsc_t d;
        lv_draw_label_dsc_init(&d);
        d.text = s_name;
        d.font = &lv_font_montserrat_20;
        d.color = lv_color_hex(COL_DIM);
        d.opa = (lv_opa_t)(255.0f * name);
        d.align = LV_TEXT_ALIGN_CENTER;
        d.letter_space = UI_SC(5);
        lv_area_t area = {CX - UI_SC(150), CY + UI_SC(78), CX + UI_SC(150), CY + UI_SC(78) + UI_SC(26)};
        lv_draw_label(l, &d, &area);
    }
}

static void timer_cb(lv_timer_t *tm)
{
    (void)tm;
    if (!s_obj) return;
    if (s_sleeping) {
        // Keep drawing: the caller cuts the power when its own timer says so.
        lv_obj_invalidate(s_obj);
        return;
    }
    float t = s_started ? (float)((esp_timer_get_time() - s_t0) / 1000) * s_speed : 0.0f;
    if (t >= T_END) {
        if (!s_done) {
            s_done = true;
            if (s_on_done) s_on_done();
        }
        return;
    }
    lv_obj_invalidate(s_obj);
}

void boot_view_create(lv_obj_t *parent, bool quick, const char *name, void (*on_done)(void))
{
    s_on_done = on_done;
    s_done = false;
    s_started = false;
    s_sleeping = false;
    s_speed = quick ? QUICK : 1.0f;
    snprintf(s_name, sizeof(s_name), "%s", name ? name : "");

    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(parent, false);

    s_obj = lv_obj_create(parent);
    lv_obj_remove_style_all(s_obj);
    lv_obj_set_size(s_obj, BOARD_LCD_W, BOARD_LCD_H);
    lv_obj_set_clickable(s_obj, false);
    lv_obj_add_event_cb(s_obj, draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    s_t0 = esp_timer_get_time();
    s_timer = lv_timer_create(timer_cb, FRAME_MS, NULL);
}

void boot_view_destroy(void)
{
    if (s_timer) lv_timer_delete(s_timer);
    s_timer = NULL;
    s_obj = NULL;  // deleted by the caller cleaning the screen
    s_on_done = NULL;
}

void boot_view_play_sleep(lv_obj_t *parent, const char *name)
{
    if (s_timer) lv_timer_delete(s_timer);
    s_on_done = NULL;
    s_done = true;
    s_started = false;
    s_sleeping = true;
    s_speed = 1.0f;
    snprintf(s_name, sizeof(s_name), "%s", name ? name : "");

    // On top of everything, and taking the touches: nothing under it should
    // react while the device is on its way down.
    s_obj = lv_obj_create(parent);
    lv_obj_remove_style_all(s_obj);
    lv_obj_set_size(s_obj, BOARD_LCD_W, BOARD_LCD_H);
    lv_obj_set_pos(s_obj, 0, 0);
    lv_obj_set_clickable(s_obj, true);
    lv_obj_add_event_cb(s_obj, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_move_foreground(s_obj);

    s_t0 = esp_timer_get_time();
    s_timer = lv_timer_create(timer_cb, FRAME_MS, NULL);
}
