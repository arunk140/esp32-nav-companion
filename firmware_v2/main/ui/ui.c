#include "ui/ui.h"
#include "ui/qr.h"
#include "gfx/gfx.h"
#include "input/touch.h"
#include "map/mapview.h"
#include "ble/ble_service.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freeze_test.h"
#include "esp_log.h"

static const char *TAG = "ui";
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Palette.
#define C_BG GFX_RGB(0x1a, 0x1a, 0x1a)
#define C_PANEL GFX_RGB(0x24, 0x24, 0x27)
#define C_FG GFX_RGB(0xf4, 0xf4, 0xf5)
#define C_DIM GFX_RGB(0x8a, 0x8a, 0x92)
#define C_ACCENT GFX_RGB(0x25, 0x63, 0xeb)
#define C_WARN GFX_RGB(0xf5, 0x9e, 0x0b)
#define C_OK GFX_RGB(0x22, 0xc5, 0x5e)

#define STATUS_H 30

static ui_screen_t s_screen = UI_PAIRING;
static bool s_linked;

// --- buttons ---------------------------------------------------------------
// All inside the round safe area (radius 225 from centre).
typedef struct {
    int x, y, w, h, r;
    const char *glyph;
    int kind;  // 0 = zoom in, 1 = zoom out, 2 = recenter
} btn_t;

static const btn_t BTN[] = {
    {392, 150, 56, 56, 28, "+", 0},
    {392, 218, 56, 56, 28, "-", 1},
    {212, 400, 56, 56, 28, "C", 2},
};
#define NBTN ((int)(sizeof(BTN) / sizeof(BTN[0])))

static bool hit(const btn_t *b, int x, int y)
{
    if (x < b->x || y < b->y || x >= b->x + b->w || y >= b->y + b->h) return false;
    // Round the corners off so a press just outside the disc does not count.
    int r = b->r;
    int cx = x < b->x + r ? b->x + r : (x > b->x + b->w - 1 - r ? b->x + b->w - 1 - r : x);
    int cy = y < b->y + r ? b->y + r : (y > b->y + b->h - 1 - r ? b->y + b->h - 1 - r : y);
    int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

static void draw_btn(const btn_t *b, bool hot)
{
    gfx_round_rect(b->x, b->y, b->w, b->h, b->r, hot ? C_ACCENT : C_PANEL);
    int gs = 3;
    gfx_text_centred(b->x + b->w / 2, b->y + (b->h - 8 * gs) / 2, b->glyph, C_FG, gs);
    gfx_dirty(b->x - 2, b->y - 2, b->w + 4, b->h + 4);
}

static void draw_buttons(int hot)
{
    for (int i = 0; i < NBTN; i++) draw_btn(&BTN[i], i == hot);
}

// --- gesture recogniser ----------------------------------------------------
// One finger: move past the slop => pan. Lift quickly without moving past the
// slop => tap. Two fingers: pinch, and the pan is suppressed for the whole
// gesture.
#define SLOP_PX 8
#define TAP_MS 500

typedef struct {
    bool tracking;      // a one-finger gesture is in progress
    bool pinching;
    int start_x, start_y;
    int last_x, last_y;
    int base_dist;      // finger separation when the pinch started
    uint32_t start_ms;
} gest_t;

static gest_t s_g;
static int s_hot_btn = -1;

static void on_tap(int x, int y)
{
    for (int i = 0; i < NBTN; i++) {
        if (hit(&BTN[i], x, y)) {
            switch (BTN[i].kind) {
            case 0: mapview_zoom_step(+1); break;
            case 1: mapview_zoom_step(-1); break;
            case 2: mapview_recenter(); break;
            }
            return;
        }
    }
}

static void gesture(const touch_sample_t *t, uint32_t now_ms)
{
    // Release: end whatever we were doing, and treat a short still one-finger
    // gesture as a tap.
    if (t->just_up) {
        if (s_g.tracking) {
            if (s_hot_btn >= 0) {
                // A press that began on a button is a button press however
                // long it was held: only a pan cancels it, and that is handled
                // in the movement path below.
                on_tap(s_g.start_x, s_g.start_y);
            } else if (now_ms - s_g.start_ms < TAP_MS) {
                int dx = t->x[0] - s_g.start_x, dy = t->y[0] - s_g.start_y;
                if (dx * dx + dy * dy <= SLOP_PX * SLOP_PX)
                    on_tap(s_g.start_x, s_g.start_y);
            }
        }
        s_g.tracking = false;
        s_g.pinching = false;
        s_g.base_dist = 0;
        s_hot_btn = -1;
        return;
    }
    if (!t->down) {
        s_g.tracking = false;
        s_g.pinching = false;
        return;
    }

    if (t->n >= 2) {
        // Pinch. Anchor on the midpoint; mapview_pinch rate-limits the step.
        if (!s_g.pinching) {
            s_g.pinching = true;
            s_g.tracking = false;
            s_g.base_dist = abs(t->x[0] - t->x[1]) + abs(t->y[0] - t->y[1]);
        } else {
            int d = abs(t->x[0] - t->x[1]) + abs(t->y[0] - t->y[1]);
            int dd = d - s_g.base_dist;
            if (dd > 50) {
                mapview_pinch(+1, (t->x[0] + t->x[1]) / 2, (t->y[0] + t->y[1]) / 2);
                s_g.base_dist = d;
            } else if (dd < -50) {
                mapview_pinch(-1, (t->x[0] + t->x[1]) / 2, (t->y[0] + t->y[1]) / 2);
                s_g.base_dist = d;
            }
        }
        return;
    }
    s_g.pinching = false;

    if (t->just_down) {
        s_g.tracking = true;
        s_g.start_x = s_g.last_x = t->x[0];
        s_g.start_y = s_g.last_y = t->y[0];
        s_g.start_ms = now_ms;
        s_hot_btn = -1;
        for (int i = 0; i < NBTN; i++)
            if (hit(&BTN[i], t->x[0], t->y[0])) s_hot_btn = i;
        return;
    }
    if (!s_g.tracking) return;

    int dx = t->x[0] - s_g.start_x, dy = t->y[0] - s_g.start_y;
    if (dx * dx + dy * dy > SLOP_PX * SLOP_PX) {
        // Past the slop it is a pan, and a press that began on a button is
        // not: that was a mis-aimed tap, not a drag.
        if (s_hot_btn >= 0) {
            s_g.tracking = false;
            s_hot_btn = -1;
            return;
        }
        mapview_pan(t->x[0] - s_g.last_x, t->y[0] - s_g.last_y);
    }
    s_g.last_x = t->x[0];
    s_g.last_y = t->y[0];
}

// --- status line -----------------------------------------------------------
static void draw_status(void)
{
    gfx_fill_rect(0, 0, GFX_W, STATUS_H, C_PANEL);
    const map_status_t *st = mapview_status();
    char line[32];
    if (st->have_fix) {
        snprintf(line, sizeof(line), "GPS %d KM/H", (st->speed_cms * 36 + 500) / 1000);
        gfx_text(6, 10, line, C_OK, 2);
    } else if (st->have_stored) {
        gfx_text(6, 10, "LAST FIX", C_DIM, 2);
    } else {
        gfx_text(6, 10, "SEARCHING", C_WARN, 2);
    }
    const char *link = s_linked ? "BLE" : "NO BLE";
    gfx_text(GFX_W - 6 - gfx_text_width(link, 2), 10, link,
             s_linked ? C_OK : C_DIM, 2);
    gfx_dirty(0, 0, GFX_W, STATUS_H);
}

// --- screens ---------------------------------------------------------------
static bool s_painted;

static void paint_pairing(void)
{
    gfx_clear(C_BG);
    gfx_text_centred(240, 26, "SCAN TO PAIR", C_FG, 3);
    qr_draw(240, 96);

    uint32_t token = settings_get_token();
    char line[48];
    snprintf(line, sizeof(line), "CODE %06lu", (unsigned long)token);
    gfx_text_centred(240, 336, line, C_ACCENT, 3);
    gfx_text_centred(240, 380, settings_get_device_name(), C_DIM, 2);
    gfx_text_centred(240, 404, "HOLD BOOT TO RESET", C_DIM, 1);
    s_painted = true;
}

static void paint_map(void)
{
    gfx_clear(C_BG);
    mapview_init();
    mapview_update(xTaskGetTickCount() * portTICK_PERIOD_MS);
    gfx_dirty_all();
    draw_buttons(-1);
    draw_status();
    s_painted = true;
}

void ui_init(void)
{
    s_painted = false;
    s_g.tracking = false;
    s_g.pinching = false;
    // Reserve the status bar: the map blits full width, so without this it
    // would erase the bar on every frame it moves and the 1 Hz repaint would
    // have to fight it.
    mapview_set_viewport(0, STATUS_H, GFX_W, GFX_H - STATUS_H);
}

ui_screen_t ui_screen(void)
{
    return s_screen;
}

void ui_show_map(void)
{
    s_screen = UI_MAP;
    s_painted = false;
    s_g.tracking = false;
    s_g.pinching = false;
    mapview_init();
    mapview_update(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

void ui_show_pairing(void)
{
    s_screen = UI_PAIRING;
    s_painted = false;
    s_g.tracking = false;
    s_g.pinching = false;
    mapview_destroy();
}

void ui_set_linked(bool linked)
{
    if (linked == s_linked) return;
    s_linked = linked;
    if (s_screen == UI_MAP) s_painted = false;  // repaint the status line
}

void ui_frame(uint32_t now_ms)
{
    touch_poll();
    const touch_sample_t *t = touch_get();
    gesture(t, now_ms);

    if (s_screen == UI_MAP) {
        mapview_update(now_ms);
        // Repaint the status line on a timer rather than on every fix: a
        // speed readout does not need 1 Hz, and repainting it per fix would
        // dirty a 480x30 strip for nothing.
        static uint32_t last_status;
        if (now_ms - last_status > 1000) {
            last_status = now_ms;
            draw_status();
        }
        // Button highlight follows press/release.
        if (t->just_up || t->just_down) draw_buttons(s_hot_btn);
    }

    if (!s_painted) {
        if (s_screen == UI_MAP) paint_map();
        else paint_pairing();
    }
#if FREEZE_AFTER_ONE_FRAME
    // Render and present exactly one frame, then never touch the panel again.
    static bool done;
    if (!done) {
        gfx_present();
        done = true;
        ESP_LOGE(TAG, "FREEZE: one frame presented; panel left idle");
    }
#else
    gfx_present();
#endif
}
