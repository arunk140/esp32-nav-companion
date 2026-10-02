// nav-esp32 firmware v2 — offline map + phone GPS over BLE.
//
// Threading model (the whole point of the rewrite):
//   core 1  gui task   — sole owner of LVGL, touch I2C reads, NVS app writes
//   core 0  NimBLE     — callbacks only copy into queues
//   core 0  tile_ld    — SD reads into tile-cache slots handed to it
// Anything crossing tasks goes through a FreeRTOS queue. No shared flags.
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "ble_service.h"
#include "board.h"
#include "display.h"
#include "lcd.h"
#include "lvgl.h"
#include "map_view.h"
#include "media_art.h"
#include "media_view.h"
#include "sdcard.h"
#include "settings.h"
#include "tiles.h"
#include "touch.h"
#include "ui_pair.h"

static const char *TAG = "nav";

#define UNPAIR_HOLD_MS 5000
#define BRIGHTNESS_SAVE_MS 1500
#define TOUCH_POLL_MS 10

static void note_activity(void);
static void enter_deep_sleep(void);
static void sleep_now(void);

typedef enum { EV_LINK_UP, EV_LINK_DOWN, EV_CMD } ev_type_t;

typedef struct {
    uint8_t type;
    uint8_t code;
    uint8_t len;
    uint8_t args[7];
} app_ev_t;

static QueueHandle_t s_ev_q;   // commands + link changes, in order
static QueueHandle_t s_pos_q;    // latest position only (overwrite)
static QueueHandle_t s_media_q;  // latest media state only (overwrite)
static QueueHandle_t s_text_q;   // latest track title/artist (overwrite)

typedef enum { SCREEN_NONE, SCREEN_PAIR, SCREEN_MAP, SCREEN_MEDIA } screen_t;
static screen_t s_screen = SCREEN_NONE;
// Screen changes requested from inside LVGL events are applied by the GUI
// loop: rebuilding the screen deletes the object whose event is running.
static screen_t s_screen_req = SCREEN_NONE;
static bool s_connected;

static uint8_t s_brightness_pending;
static uint32_t s_brightness_changed_ms;
static bool s_brightness_dirty;

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ---------------------------------------------------------- BLE (host task)

static void ble_on_position(const nav_position_t *p)
{
    xQueueOverwrite(s_pos_q, p);
}

static void ble_on_cmd(uint8_t code, const uint8_t *args, size_t len)
{
    app_ev_t ev = {.type = EV_CMD, .code = code};
    if (len > sizeof(ev.args)) len = sizeof(ev.args);
    memcpy(ev.args, args, len);
    ev.len = (uint8_t)len;
    xQueueSend(s_ev_q, &ev, 0);
}

static void ble_on_media_state(const nav_media_state_t *st)
{
    xQueueOverwrite(s_media_q, st);
}

typedef struct {
    char title[72];
    char artist[56];
} text_msg_t;

static void ble_on_media_text(const char *title, const char *artist)
{
    text_msg_t m;
    snprintf(m.title, sizeof(m.title), "%s", title);
    snprintf(m.artist, sizeof(m.artist), "%s", artist);
    xQueueOverwrite(s_text_q, &m);
}

static void ble_on_link(bool up)
{
    app_ev_t ev = {.type = up ? EV_LINK_UP : EV_LINK_DOWN};
    xQueueSend(s_ev_q, &ev, 0);
}

// ---------------------------------------------------------- screens (GUI)

static void browse_cb(lv_event_t *e);
static void open_map(void);

static void show_screen(screen_t which)
{
    if (which == s_screen) return;
    lv_obj_t *scr = lv_screen_active();
    map_view_destroy();
    media_view_destroy();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    if (which == SCREEN_PAIR) {
        ui_pair_create(scr, browse_cb);
    } else if (which == SCREEN_MEDIA) {
        media_view_create(scr, open_map);
    } else {
        map_view_create(scr);
        map_view_set_link(s_connected, settings_paired());
    }
    s_screen = which;
}

static void open_map(void)
{
    s_screen_req = SCREEN_MAP;
}

static void open_media(void)
{
    s_screen_req = SCREEN_MEDIA;
}

static void browse_cb(lv_event_t *e)
{
    (void)e;
    open_map();
}

static void apply_brightness(uint8_t pct)
{
    board_backlight_set(pct < NAV_BRIGHTNESS_MIN ? NAV_BRIGHTNESS_MIN : pct);
}

// remote: the phone asked (CMD 0x04) and removes its own bond, so ours goes
// too. Local (BOOT): keep the bond — the phone still holds it, and erasing
// ours makes Android loop on "pair again" popups. A fresh token is enough:
// the old QR scan no longer pairs, so the phone has to scan again.
static void unpair(bool remote)
{
    settings_set_paired(false);
    settings_new_token();
    if (remote) {
        nav_ble_forget_current();
    } else {
        nav_ble_disconnect();
    }
    map_view_set_link(false, false);
    s_screen = SCREEN_NONE;  // rebuild the QR with the new token
    show_screen(SCREEN_PAIR);
}

static void media_changed(void)
{
    // The toast has a fixed width, so clip long titles instead of letting the
    // label run off the round screen.
    // The toast sits under the status pill, so keep it to one line: 20
    // characters, cut at a word boundary where there is one.
    const char *t = media_view_title();
    char buf[24];
    size_t n = 20;
    if (strlen(t) > n) {
        for (size_t i = n; i > 8; i--) {
            if (t[i] == ' ') {
                n = i;
                break;
            }
        }
        snprintf(buf, sizeof(buf), "%.*s...", (int)n, t);
    } else {
        snprintf(buf, sizeof(buf), "%s", t);
    }
    map_view_toast(buf);
}

static void local_reset(void)
{
    unpair(false);
}

static bool valid_fix(const nav_position_t *p)
{
    if (p->lat_e7 == 0 && p->lon_e7 == 0) return false;  // "no fix yet"
    return p->lat_e7 >= -850000000 && p->lat_e7 <= 850000000 &&
           p->lon_e7 >= -1800000000 && p->lon_e7 <= 1800000000;
}

static void handle_cmd(const app_ev_t *ev)
{
    switch (ev->code) {
    case NAV_CMD_RECENTER:
        map_view_recenter();
        break;
    case NAV_CMD_SET_ZOOM:
        if (ev->len >= 1) map_view_set_zoom(ev->args[0]);
        break;
    case NAV_CMD_PING:
        break;
    case NAV_CMD_UNPAIR:
        unpair(true);
        break;
    case NAV_CMD_SET_BRIGHTNESS:
        if (ev->len >= 1) {
            uint8_t v = ev->args[0] > 100 ? 100 : ev->args[0];
            apply_brightness(v);
            // The app streams slider moves; persist once it settles.
            s_brightness_pending = v;
            s_brightness_dirty = true;
            s_brightness_changed_ms = now_ms();
        }
        break;
    case NAV_CMD_SLEEP:
        enter_deep_sleep();
        break;
    case NAV_CMD_PAIR:
        if (ev->len >= 6 && memcmp(ev->args, settings_token_str(), 6) == 0) {
            settings_set_paired(true);
            map_view_set_link(s_connected, true);
            show_screen(SCREEN_MAP);
        } else {
            ESP_LOGW(TAG, "pair token mismatch");
        }
        break;
    default:
        break;
    }
}

static void process_events(void)
{
    app_ev_t ev;
    while (xQueueReceive(s_ev_q, &ev, 0) == pdTRUE) {
        note_activity();
        switch (ev.type) {
        case EV_LINK_UP:
            s_connected = true;
            map_view_set_link(true, settings_paired());
            media_view_set_link(true);
            break;
        case EV_LINK_DOWN:
            s_connected = false;
            map_view_set_link(false, settings_paired());
            media_view_set_link(false);
            break;
        case EV_CMD:
            handle_cmd(&ev);
            break;
        }
    }
    text_msg_t tm;
    if (xQueueReceive(s_text_q, &tm, 0) == pdTRUE) media_view_set_text(tm.title, tm.artist);
    nav_media_state_t ms;
    if (xQueueReceive(s_media_q, &ms, 0) == pdTRUE) {
        note_activity();
        media_view_set_state(&ms);
    }
    nav_position_t p;
    if (xQueueReceive(s_pos_q, &p, 0) == pdTRUE && valid_fix(&p)) {
        map_view_set_fix(p.lat_e7, p.lon_e7, p.speed_cms, p.cog_cdeg);
    }
    if (s_brightness_dirty && now_ms() - s_brightness_changed_ms > BRIGHTNESS_SAVE_MS) {
        s_brightness_dirty = false;
        settings_set_brightness(s_brightness_pending);
    }
}

static void poll_boot_button(void)
{
    static uint32_t down_ms;
    static bool fired;
    if (board_boot_button_pressed()) {
        if (!down_ms) down_ms = now_ms();
        if (!fired && now_ms() - down_ms > UNPAIR_HOLD_MS) {
            fired = true;
            unpair(false);
        }
    } else {
        down_ms = 0;
        fired = false;
    }
}

#if CONFIG_NAV_DEBUG_STATS
static void print_stats(void)
{
    static uint32_t last;
    uint32_t t = now_ms();
    if (t - last < 5000) return;
    last = t;
    tiles_stats_t ts;
    tiles_get_stats(&ts);
    ESP_LOGW(TAG, "fps %lu draw %lums wait %lums | tiles %lu avg %lums max %lums miss %lu pend %lu | "
                  "int %u KB psram %u KB | ble %d | rot %.0f",
             (unsigned long)display_fps(), (unsigned long)display_last_draw_ms(),
             (unsigned long)display_last_wait_ms(),
             (unsigned long)ts.loads, (unsigned long)ts.avg_load_ms, (unsigned long)ts.max_load_ms,
             (unsigned long)ts.missing, (unsigned long)ts.pending,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), s_connected,
             map_view_rotation());
}
#endif

#if CONFIG_NAV_DEBUG_DEMO_WALK
// Bench aid: feeds simulated fixes (1 Hz, ~25 km/h circle around the
// Gateway of India) through the same path as BLE positions.
static void demo_walk(void)
{
    static uint32_t last;
    static float ang;
    uint32_t t = now_ms();
    if (t - last < 1000) return;
    last = t;
    ang += 0.02f;
    nav_position_t p = {
        .lat_e7 = (int32_t)((18.9219 + 0.012 * sinf(ang)) * 1e7),
        .lon_e7 = (int32_t)((72.8347 + 0.012 * cosf(ang)) * 1e7),
        .speed_cms = 700,
        .cog_cdeg = (uint16_t)(fmodf(360.0f - ang * 57.2958f, 360.0f) * 100),
        .fix = 1,
    };
    xQueueOverwrite(s_pos_q, &p);
}
#endif

// ---- idle power management -------------------------------------------
// Stage 1: backlight off after a while without touch, phone or movement.
//   BLE stays up, so the phone reconnecting wakes the screen by itself.
// Stage 2: deep sleep after longer idleness, which turns the radio off, so
//   the phone cannot wake it - touch is the only way back.
static uint32_t s_last_activity;
static bool s_screen_off;
static bool s_sleep_pending;
static uint32_t s_sleep_at;
#define SLEEP_INDICATOR_MS 800

static void note_activity(void)
{
    s_last_activity = now_ms();
    if (s_screen_off) {
        s_screen_off = false;
        apply_brightness(settings_brightness());
        ESP_LOGW(TAG, "screen on");
    }
}

// Shows "Sleeping..." and commits shortly after: a tap or a phone command
// that darkens the screen with no feedback reads as a broken device.
static void enter_deep_sleep(void)
{
    if (s_sleep_pending) return;
    s_sleep_pending = true;
    s_sleep_at = now_ms() + SLEEP_INDICATOR_MS;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *dim = lv_obj_create(scr);
    lv_obj_remove_style_all(dim);
    lv_obj_set_size(dim, BOARD_LCD_W, BOARD_LCD_H);
    lv_obj_center(dim);
    lv_obj_set_style_bg_color(dim, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(dim, LV_OPA_80, 0);
    lv_obj_set_clickable(dim, false);
    lv_obj_t *msg = lv_label_create(dim);
    lv_label_set_text(msg, "Sleeping...");
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(msg, lv_color_white(), 0);
    lv_obj_center(msg);
    ESP_LOGW(TAG, "sleeping");
}

static void sleep_now(void)
{
    // Persist what the GUI task has been deferring, then go dark.
    map_view_flush_state();
    if (s_brightness_dirty) settings_set_brightness(s_brightness_pending);
    board_backlight_set(0);
    board_exio_set(EXIO_LCD_RST, false);  // panel controller into reset
    touch_poll();                          // clear any pending frame: INT high
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGW(TAG, "deep sleep (touch to wake)");
    // Touch INT (GPIO16) is the only usable wake source: it is an RTC pin.
    // BOOT (GPIO0) would also wake, but holding it across the reset that
    // follows would strap the chip into the bootloader.
    esp_sleep_enable_ext1_wakeup_io(BIT64(16), ESP_EXT1_WAKEUP_ALL_LOW);
    esp_deep_sleep_start();
}

static void sleep_tick(void)
{
    if (s_sleep_pending && (int32_t)(now_ms() - s_sleep_at) >= 0) {
        s_sleep_pending = false;
        sleep_now();
    }
}

static void power_tick(void)
{
#if CONFIG_NAV_SLEEP_ENABLE
    // A live link means someone is using the device: never sleep. Timers
    // start counting again from the moment the phone drops.
    if (s_connected) return;
    uint32_t idle = now_ms() - s_last_activity;
    if (s_screen_off) {
        if (idle >= CONFIG_NAV_DEEP_SLEEP_MS) enter_deep_sleep();
    } else if (idle >= CONFIG_NAV_SCREEN_OFF_MS) {
        s_screen_off = true;
        board_backlight_set(0);
        ESP_LOGW(TAG, "screen off");
    }
#endif
}

static void gui_task(void *arg)
{
    (void)arg;
    bool backlight_on = false;
    uint32_t last_touch = 0, last_fps_pub = 0;
    for (;;) {
        uint32_t t = now_ms();
        if (t - last_touch >= TOUCH_POLL_MS) {
            last_touch = t;
            touch_poll();
            if (touch_get()->n) note_activity();
            // Never sleep while moving: dim instead.
            if (map_view_is_moving()) note_activity();
        }
#if CONFIG_NAV_DEBUG_DEMO_WALK
        demo_walk();
#endif
        process_events();
        poll_boot_button();
        power_tick();
        sleep_tick();
        map_view_tick();
        media_view_tick();
        if (s_screen_req != SCREEN_NONE) {
            screen_t req = s_screen_req;
            s_screen_req = SCREEN_NONE;
            show_screen(req);
        }

        uint32_t wait = lv_timer_handler();

        if (!backlight_on) {
            // First frame is on the glass: no boot garbage visible.
            backlight_on = true;
            apply_brightness(settings_brightness());
            note_activity();
        }
        if (t - last_fps_pub > 1000) {
            last_fps_pub = t;
            nav_ble_set_fps((uint8_t)(display_fps() > 255 ? 255 : display_fps()));
        }
#if CONFIG_NAV_DEBUG_STATS
        print_stats();
#endif
        if (wait > 5) wait = 5;
        if (wait < 1) wait = 1;
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(settings_init());

    // Init order is load-bearing: I2C -> expander -> LCD -> touch -> SD.
    // The expander holds LCD/touch reset + LCD/SD chip selects; the LCD's
    // init SPI shares GPIO1/2 with the SD card and must run (and release
    // the pins) before the SD card is mounted.
    ESP_ERROR_CHECK(board_i2c_init());
    ESP_ERROR_CHECK(board_exio_init());
    board_backlight_init();  // off until the first frame
    ESP_ERROR_CHECK(lcd_init());
    if (touch_init() != ESP_OK) ESP_LOGE(TAG, "touch unavailable");
    sdcard_init();  // failure is shown on screen
    board_boot_button_init();

    tiles_init();
    display_init();
    map_view_init_state();
    map_view_set_reset_cb(local_reset);
    map_view_set_media_cb(open_media);
    map_view_set_sleep_cb(enter_deep_sleep);
    media_view_set_change_cb(media_changed);
    media_art_init();

    s_ev_q = xQueueCreate(16, sizeof(app_ev_t));
    s_pos_q = xQueueCreate(1, sizeof(nav_position_t));
    s_media_q = xQueueCreate(1, sizeof(nav_media_state_t));
    s_text_q = xQueueCreate(1, sizeof(text_msg_t));

    show_screen(settings_paired() ? SCREEN_MAP : SCREEN_PAIR);

    static const nav_ble_cbs_t cbs = {
        .on_position = ble_on_position,
        .on_cmd = ble_on_cmd,
        .on_link = ble_on_link,
        .on_media_state = ble_on_media_state,
        .on_media_art = media_art_feed,
        .on_media_text = ble_on_media_text,
    };
    nav_ble_start(&cbs, settings_device_name());

    xTaskCreatePinnedToCore(gui_task, "gui", 10 * 1024, NULL, 5, NULL, 1);
}
