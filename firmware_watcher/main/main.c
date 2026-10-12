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
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "ble_service.h"
#include "board.h"
#include "call_view.h"
#include "boot_view.h"
#include "display.h"
#include "lcd.h"
#include "lvgl.h"
#include "map_view.h"
#include "media_art.h"
#include "media_view.h"
#include "nav_view.h"
#include "sdcard.h"
#include "settings.h"
#include "tiles.h"
#include "touch.h"
#include "ui_pair.h"

static const char *TAG = "nav";

#define UNPAIR_HOLD_MS 5000
#define BRIGHTNESS_SAVE_MS 1500
#define TOUCH_POLL_MS 10
#define BATTERY_POLL_MS 10000
#define KNOB_BRIGHTNESS_STEP 5    // percent per wheel step
#define KNOB_VOLUME_MAX_STEPS 8   // per pass; the BLE command queue is short
#define KNOB_VOLUME_TOAST_MS 1500 // how long after a turn a volume report is "ours"

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

// Nav STATE, TEXT and DEST arrive as separate writes, so merge them: a state
// that lands between two text messages must not be lost, the street name has
// to stay sticky, and the destination is latest-wins.
typedef struct {
    bool have_state;
    nav_state_t st;
    char street[NAV_TEXT_MAX + 1];
    bool have_street;
    bool have_dest;
    bool dest_present;
    int32_t dest_lat_e7, dest_lon_e7;
} nav_msg_t;
static QueueHandle_t s_nav_q;    // nav state + street + destination (overwrite)

// Route geometry chunks: ordered, unlike the overwrite-latest nav slot. The
// BLE callback only queues; the GUI task assembles into map_view.
typedef struct {
    uint8_t flags, seq, count;
    int32_t lat[NAV_ROUTE_CHUNK_MAX];
    int32_t lon[NAV_ROUTE_CHUNK_MAX];
} route_chunk_t;
static QueueHandle_t s_route_q;
static uint8_t s_route_seq;
static bool s_route_bad;

// Incoming call. One overwrite-latest slot: a ring that is immediately
// followed by an end must not leave a stale screen up.
typedef struct {
    bool ringing;
    char name[NAV_CALL_TEXT_MAX + 1];
    char number[NAV_CALL_TEXT_MAX + 1];
} call_msg_t;
static QueueHandle_t s_call_q;

typedef enum { SCREEN_NONE, SCREEN_PAIR, SCREEN_MAP, SCREEN_MEDIA, SCREEN_CALL, SCREEN_BOOT } screen_t;
static screen_t s_screen = SCREEN_NONE;
// Screen changes requested from inside LVGL events are applied by the GUI
// loop: rebuilding the screen deletes the object whose event is running.
static screen_t s_screen_req = SCREEN_NONE;
#define MEDIA_IDLE_BACK_MS 15000
static uint32_t s_media_touch_ms;  // last touch while the music controls were up
// Where to return after the call page closes (map or media page).
static screen_t s_screen_before_call = SCREEN_MAP;
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

static void ble_on_call_ring(const char *name, const char *number)
{
    call_msg_t m = {.ringing = true};
    snprintf(m.name, sizeof(m.name), "%s", name);
    snprintf(m.number, sizeof(m.number), "%s", number);
    xQueueOverwrite(s_call_q, &m);
}

static void ble_on_call_end(void)
{
    call_msg_t m = {.ringing = false};
    xQueueOverwrite(s_call_q, &m);
}

static void ble_on_nav_state(const nav_state_t *st, const char *street)
{
    static nav_msg_t m;
    if (st) {
        m.st = *st;
        m.have_state = true;
    }
    if (street) {
        snprintf(m.street, sizeof(m.street), "%s", street);
        m.have_street = true;
    }
    if (m.have_state || m.have_street) xQueueOverwrite(s_nav_q, &m);
}

static void ble_on_nav_dest(bool present, int32_t lat_e7, int32_t lon_e7)
{
    static nav_msg_t m;
    m.have_dest = true;
    m.dest_present = present;
    m.dest_lat_e7 = lat_e7;
    m.dest_lon_e7 = lon_e7;
    xQueueOverwrite(s_nav_q, &m);
}

// The limit is two small values, latest wins: no queue, the GUI loop picks
// them up. Written in the NimBLE host task, read in the GUI task.
static volatile bool s_limit_dirty, s_limit_mph;
static volatile uint16_t s_limit_val;

static volatile bool s_phone_dirty, s_phone_charging, s_phone_24h;
static volatile int s_phone_batt;
static volatile uint32_t s_phone_epoch;

static void ble_on_phone_info(int battery_pct, bool charging, bool clock_24h, uint32_t local_epoch_s)
{
    s_phone_24h = clock_24h;
    s_phone_batt = battery_pct;
    s_phone_charging = charging;
    s_phone_epoch = local_epoch_s;
    s_phone_dirty = true;
}

static void ble_on_nav_limit(bool mph, uint16_t limit)
{
    s_limit_mph = mph;
    s_limit_val = limit;
    s_limit_dirty = true;
}

// Nearby-street chunks: ordered like the route's. The parsing is map_view's.
typedef struct {
    uint8_t len;
    uint8_t data[NAV_ROADS_CHUNK_MAX];
} roads_chunk_t;
static QueueHandle_t s_roads_q;

static void ble_on_roads_chunk(const uint8_t *data, size_t len)
{
    if (len > NAV_ROADS_CHUNK_MAX) return;
    roads_chunk_t c = {.len = (uint8_t)len};
    memcpy(c.data, data, len);
    // A full queue drops the chunk; map_view sees the gap in seq and throws
    // the whole set away, so a partial set is never drawn.
    xQueueSend(s_roads_q, &c, 0);
}

// Congestion for the route: one message, latest wins. It is drained after the
// route chunks in the same pass, so it always lands on the route it came with.
static QueueHandle_t s_traffic_q;

static void ble_on_traffic(const uint8_t *data, size_t len)
{
    roads_chunk_t c = {.len = (uint8_t)len};
    memcpy(c.data, data, len);
    xQueueOverwrite(s_traffic_q, &c);
}

static void ble_on_route_chunk(uint8_t flags, uint8_t seq, uint8_t count,
                               const int32_t *lat_e7, const int32_t *lon_e7)
{
    if (count > NAV_ROUTE_CHUNK_MAX) return;
    route_chunk_t c = {.flags = flags, .seq = seq, .count = count};
    memcpy(c.lat, lat_e7, (size_t)count * sizeof(int32_t));
    memcpy(c.lon, lon_e7, (size_t)count * sizeof(int32_t));
    // Ordered: a full queue drops a chunk, the seq check below then discards
    // the whole route rather than drawing a gap. The phone re-sends on reroute.
    xQueueSend(s_route_q, &c, 0);
}

static void ble_on_link(bool up)
{
    app_ev_t ev = {.type = up ? EV_LINK_UP : EV_LINK_DOWN};
    xQueueSend(s_ev_q, &ev, 0);
}

// ---------------------------------------------------------- screens (GUI)

static void browse_cb(lv_event_t *e);
static void open_map(void);
static void return_from_call(void);

static void call_answer_cb(void)
{
#if CONFIG_NAV_DEBUG_BLE
    ESP_LOGW(TAG, "call: answer");
#endif
    nav_ble_call_cmd(NAV_CCMD_ANSWER);
    return_from_call();
}

static void call_reject_cb(void)
{
#if CONFIG_NAV_DEBUG_BLE
    ESP_LOGW(TAG, "call: reject");
#endif
    nav_ble_call_cmd(NAV_CCMD_REJECT);
    return_from_call();
}

static void call_dismiss_cb(void)
{
#if CONFIG_NAV_DEBUG_BLE
    ESP_LOGW(TAG, "call: dismiss");
#endif
    return_from_call();
}

// The start-up animation has played: on to the screen the device starts on.
// Runs from an LVGL timer, so it only asks.
static void boot_done(void)
{
    if (s_screen == SCREEN_BOOT && s_screen_req == SCREEN_NONE) {
        s_screen_req = settings_paired() ? SCREEN_MAP : SCREEN_PAIR;
    }
}

static void show_screen(screen_t which)
{
    if (which == s_screen) return;
    lv_obj_t *scr = lv_screen_active();
    map_view_destroy();
    media_view_destroy();
    nav_view_destroy();
    call_view_destroy();
    boot_view_destroy();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    if (which == SCREEN_BOOT) {
        // Waking from deep sleep restarts the chip like a power-on does, and
        // plays the same animation at the same speed.
        boot_view_create(scr, false, settings_device_name(), boot_done);
    } else if (which == SCREEN_PAIR) {
        ui_pair_create(scr, browse_cb);
    } else if (which == SCREEN_MEDIA) {
        s_media_touch_ms = now_ms();  // opening it counts as touching it
        media_view_create(scr, open_map);
    } else if (which == SCREEN_CALL) {
        call_view_create(scr, call_answer_cb, call_reject_cb, call_dismiss_cb);
    } else {
        map_view_create(scr);
        map_view_set_link(s_connected, settings_paired());
        nav_view_init();  // banner reappears if navigation is already live
    }
#if CONFIG_NAV_DEBUG_BLE
    ESP_LOGW(TAG, "screen -> %d (connected=%d paired=%d)", (int)which, s_connected,
             settings_paired());
#endif
    s_screen = which;
}

// Dismiss the call page and go back where the user was: the map, or the media
// page if the call interrupted it. Called from an LVGL click event, so the
// screen swap itself is deferred to the GUI loop.
static void return_from_call(void)
{
    s_screen_req = (s_screen_before_call == SCREEN_MEDIA) ? SCREEN_MEDIA : SCREEN_MAP;
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

// Theme change: cached tiles are dropped by tiles_set_dark() and re-read, so
// the repaint below is only for the placeholder colours.
// Minimal display on or off, from the phone, a long press on the map, or the
// stored setting at boot.
static void set_minimal(bool on)
{
    settings_set_minimal(on);
    map_view_set_minimal(on);
    nav_view_set_minimal(on);
    nav_ble_set_minimal(on);
}

static void toggle_minimal(void)
{
    bool on = !settings_minimal();
    set_minimal(on);
    map_view_toast(on ? "Minimal display" : "Map");
}

static uint32_t s_knob_vol_ms;  // last wheel turn sent as volume; 0 = never

// Brightness from the device's own screen (the battery face). Returns the
// level now in use; a delta of 0 only asks.
static int local_brightness(int delta)
{
    int v = s_brightness_dirty ? s_brightness_pending : settings_brightness();
    if (!delta) return v;
    v += delta;
    if (v < NAV_BRIGHTNESS_MIN) v = NAV_BRIGHTNESS_MIN;
    if (v > 100) v = 100;
    apply_brightness((uint8_t)v);
    s_brightness_pending = (uint8_t)v;
    s_brightness_dirty = true;
    s_brightness_changed_ms = now_ms();
    return v;
}

// The wheel: brightness while the battery/brightness face is up, the phone's
// media volume everywhere else.
static void poll_knob(void)
{
    int steps = board_knob_steps();
    if (!steps) return;
    note_activity();
    if (s_screen == SCREEN_MAP && map_view_brightness_face()) {
        local_brightness(steps * KNOB_BRIGHTNESS_STEP);
        return;
    }
    if (!nav_ble_media_ready()) return;
    int n = steps < 0 ? -steps : steps;
    if (n > KNOB_VOLUME_MAX_STEPS) n = KNOB_VOLUME_MAX_STEPS;
    while (n--) nav_ble_media_cmd(steps > 0 ? NAV_MCMD_VOL_UP : NAV_MCMD_VOL_DOWN, 0);
    s_knob_vol_ms = now_ms() ? now_ms() : 1;
}

static void set_theme(bool dark)
{
    tiles_set_dark(dark);
    settings_set_dark_map(dark);
    nav_ble_set_theme(dark);
    map_view_invalidate();
    ESP_LOGW(TAG, "%s map", dark ? "dark" : "light");
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
    case NAV_CMD_SET_THEME:
        if (ev->len >= 1) set_theme(ev->args[0] != 0);
        break;
    case NAV_CMD_SET_MINIMAL:
        if (ev->len >= 1) set_minimal(ev->args[0] != 0);
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

// Assemble queued route chunks in order. Runs in the GUI task so map_view is
// touched from one task only.
static void process_route(void)
{
    route_chunk_t c;
    while (xQueueReceive(s_route_q, &c, 0) == pdTRUE) {
        if (c.flags & NAV_ROUTE_F_START) {
            s_route_seq = 0;
            s_route_bad = false;
            map_view_route_reset();
        }
        if (s_route_bad) continue;
        if (c.seq != s_route_seq) {
            s_route_bad = true;
            map_view_route_clear();
            continue;
        }
        s_route_seq++;
        if (c.count) map_view_route_add(c.lat, c.lon, c.count);
        if ((c.flags & NAV_ROUTE_F_END) && !s_route_bad) map_view_route_commit();
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
            // The phone is the navigation source: no link means no guidance
            // and no destination.
            nav_view_clear();
            map_view_set_dest(false, 0, 0);
            map_view_route_clear();
            // A dropped link also ends any call UI: the phone can no longer
            // update it, and the page must not linger.
            call_view_clear();
            if (s_screen == SCREEN_CALL || s_screen_req == SCREEN_CALL) {
                return_from_call();
            }
            break;
        case EV_CMD:
            handle_cmd(&ev);
            break;
        }
    }
    nav_msg_t nm;
    if (xQueueReceive(s_nav_q, &nm, 0) == pdTRUE) {
        note_activity();
        if (nm.have_street) nav_view_update(NULL, nm.street);
        if (nm.have_state) {
            nav_view_update(&nm.st, NULL);
            // Navigation stopped -> the destination pin goes with it. (No
            // point waiting for the next DEST with present=0; arrival may
            // never send one.)
            if (!(nm.st.flags & NAV_F_ACTIVE)) {
                map_view_set_dest(false, 0, 0);
                map_view_route_clear();
            }
        }
        if (nm.have_dest) map_view_set_dest(nm.dest_present, nm.dest_lat_e7, nm.dest_lon_e7);
    }
    if (s_phone_dirty) {
        s_phone_dirty = false;
        map_view_set_phone(s_phone_batt, s_phone_charging, s_phone_24h, s_phone_epoch);
    }
    if (s_limit_dirty) {
        s_limit_dirty = false;
        nav_view_set_limit(s_limit_mph, s_limit_val);
        map_view_invalidate();
    }
    process_route();
    roads_chunk_t rc;
    while (xQueueReceive(s_roads_q, &rc, 0) == pdTRUE) map_view_roads_chunk(rc.data, rc.len);
    if (xQueueReceive(s_traffic_q, &rc, 0) == pdTRUE) map_view_traffic(rc.data, rc.len);
    text_msg_t tm;
    if (xQueueReceive(s_text_q, &tm, 0) == pdTRUE) media_view_set_text(tm.title, tm.artist);
    nav_media_state_t ms;
    if (xQueueReceive(s_media_q, &ms, 0) == pdTRUE) {
        note_activity();
        media_view_set_state(&ms);
        // The music screen has its own volume arc; everywhere else the phone's
        // answer to a turn of the wheel is the only feedback there is.
        if (s_knob_vol_ms && now_ms() - s_knob_vol_ms < KNOB_VOLUME_TOAST_MS && s_screen == SCREEN_MAP) {
            char buf[20];
            snprintf(buf, sizeof(buf), "Volume %d%%", ms.volume);
            map_view_toast(buf);
        }
    }
    nav_position_t p;
    if (xQueueReceive(s_pos_q, &p, 0) == pdTRUE && valid_fix(&p)) {
        map_view_set_fix(p.lat_e7, p.lon_e7, p.speed_cms, p.cog_cdeg);
    }
    call_msg_t cm;
    if (xQueueReceive(s_call_q, &cm, 0) == pdTRUE) {
        note_activity();
        if (cm.ringing) {
#if CONFIG_NAV_DEBUG_BLE
            ESP_LOGW(TAG, "call ring: name='%s' number='%s' screen=%d", cm.name, cm.number,
                     (int)s_screen);
#endif
            // Take over the map or media page; never interrupt pairing. A
            // ring that arrives after the link dropped is stale: ignore it.
            if (s_connected &&
                (s_screen == SCREEN_MAP || s_screen == SCREEN_MEDIA ||
                 s_screen == SCREEN_CALL)) {
                call_view_set_caller(cm.name, cm.number);
                if (s_screen != SCREEN_CALL) s_screen_before_call = s_screen;
                s_screen_req = SCREEN_CALL;
            }
        } else {
#if CONFIG_NAV_DEBUG_BLE
            ESP_LOGW(TAG, "call end (screen=%d)", (int)s_screen);
#endif
            if (s_screen == SCREEN_CALL || s_screen_req == SCREEN_CALL) {
                return_from_call();
            }
        }
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
    ESP_LOGW(TAG, "fps %lu draw %lums xfer %lums | tiles %lu avg %lums max %lums miss %lu pend %lu | "
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
    s_sleep_at = now_ms() + BOOT_VIEW_SLEEP_MS;

    // The start-up animation run backwards, over whatever is showing.
    boot_view_play_sleep(lv_screen_active(), settings_device_name());
    ESP_LOGW(TAG, "sleeping");
}

static void sleep_now(void)
{
    // Persist what the GUI task has been deferring, then go dark.
    map_view_flush_state();
    if (s_brightness_dirty) settings_set_brightness(s_brightness_pending);
    board_backlight_set(0);
    touch_poll();                          // drain the controller: INT idle high
    vTaskDelay(pdMS_TO_TICKS(50));         // and let the last frame leave the bus
    // Backlight and panel bus low and latched, then the rail: nothing may be
    // left driving, or floating into, a panel that has lost its supply.
    board_sleep_pins_hold();
    // Drop the peripheral rails (LCD, SD, codec, Grove). The panel and card are
    // re-powered by board_exio_init() on the reset that follows the wake.
    board_power_rails_off();
    // The wake line is the expander's interrupt, which asserts on ANY input
    // change and stays asserted until the inputs are read. Dropping the rails
    // changes several inputs at once (the touch interrupt falls with the LCD
    // rail), and so does letting go of the wheel after the long press that
    // asked for this - either one left pending wakes the chip the instant it
    // sleeps. So: let the rails settle, wait for the wheel to be released,
    // then read the inputs, which clears the interrupt.
    vTaskDelay(pdMS_TO_TICKS(100));
    for (int i = 0; i < 100 && board_boot_button_pressed(); i++) vTaskDelay(pdMS_TO_TICKS(30));
    vTaskDelay(pdMS_TO_TICKS(30));
    board_exio_get(0);
    ESP_LOGW(TAG, "deep sleep (wheel to wake)");
    // Wake source: the IO expander's shared interrupt on GPIO2, active low.
    // GPIO2 is RTC-capable, and the expander's INT is open-drain, so the RTC
    // pull-up holds it high across the sleep.
    //
    // Note this is NOT the tap-to-wake of the Waveshare board: the sleep mask
    // powers down the panel and its touch controller along with the LCD rail,
    // so while asleep the only things that can assert INT are the wheel button
    // and a charger state change. To experiment with touch-to-wake, keep
    // EXIO_PWR_LCD up in board_power_rails_off() and accept the sleep current.
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_2, 0);
    rtc_gpio_pullup_en(GPIO_NUM_2);
    rtc_gpio_pulldown_dis(GPIO_NUM_2);
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
    uint32_t last_touch = 0, last_fps_pub = 0, last_batt = 0;
    for (;;) {
        uint32_t t = now_ms();
        if (t - last_touch >= TOUCH_POLL_MS) {
            last_touch = t;
            touch_poll();
            if (touch_get()->n) {
                note_activity();
                s_media_touch_ms = t;
            }
            // Never sleep while moving: dim instead.
            if (map_view_is_moving()) note_activity();
        }
#if CONFIG_NAV_DEBUG_DEMO_WALK
        demo_walk();
#endif
        process_events();
        poll_boot_button();
        poll_knob();
        power_tick();
        sleep_tick();
        map_view_tick();
        media_view_tick();
        // During a drive the music controls are a detour: left untouched for
        // MEDIA_IDLE_BACK_MS they give the screen back to the navigation.
        if (s_screen == SCREEN_MEDIA && s_screen_req == SCREEN_NONE && nav_view_active() &&
            t - s_media_touch_ms >= MEDIA_IDLE_BACK_MS) {
            s_screen_req = SCREEN_MAP;
        }
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
        if (!last_batt || t - last_batt >= BATTERY_POLL_MS) {
            bool first = !last_batt;
            last_batt = t ? t : 1;
            int pct = -1;
            bool charging = false;
            if (!board_battery_read(&pct, &charging)) pct = -1;
            if (first) ESP_LOGW(TAG, "battery: %d%%%s", pct, pct < 0 ? " (unavailable)" : charging ? " on USB" : "");
            map_view_set_device_batt(pct, charging);
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

    // Init order is load-bearing. On this board the expander is not merely a
    // pin expander: it switches the LCD and SD power rails, so it has to be up
    // before either. After that the panel (SPI3) and the card (SPI2) are
    // independent — unlike the Waveshare board, where the card and the panel's
    // init SPI shared GPIO1/2 and had to be sequenced.
    board_sleep_pins_release();  // latched low across the sleep we woke from
    ESP_ERROR_CHECK(board_i2c_init());
    ESP_ERROR_CHECK(board_exio_init());  // raises the LCD + SD rails
    board_backlight_init();              // off until the first frame
    ESP_ERROR_CHECK(lcd_init());
    if (touch_init() != ESP_OK) ESP_LOGE(TAG, "touch unavailable");
    sdcard_init();  // failure is shown on screen
    board_boot_button_init();
    board_knob_init();

    tiles_init();
    // Stored preference wins over the build-time default.
    set_theme(settings_dark_map());
    display_init();
    map_view_init_state();
    set_minimal(settings_minimal());
    map_view_set_mode_cb(toggle_minimal);
    map_view_set_brightness_cb(local_brightness);
    map_view_set_reset_cb(local_reset);
    map_view_set_media_cb(open_media);
    map_view_set_sleep_cb(enter_deep_sleep);
    // Tapping the nav banner re-centres, same as the recentre button (which is
    // hidden while the banner is up).
    nav_view_set_click_cb(map_view_recenter);
    media_view_set_change_cb(media_changed);
    media_art_init();

    s_ev_q = xQueueCreate(16, sizeof(app_ev_t));
    s_pos_q = xQueueCreate(1, sizeof(nav_position_t));
    s_media_q = xQueueCreate(1, sizeof(nav_media_state_t));
    s_text_q = xQueueCreate(1, sizeof(text_msg_t));
    s_nav_q = xQueueCreate(1, sizeof(nav_msg_t));
    s_call_q = xQueueCreate(1, sizeof(call_msg_t));
    s_route_q = xQueueCreate(8, sizeof(route_chunk_t));
    s_roads_q = xQueueCreate(16, sizeof(roads_chunk_t));
    s_traffic_q = xQueueCreate(1, sizeof(roads_chunk_t));

    show_screen(SCREEN_BOOT);

    static const nav_ble_cbs_t cbs = {
        .on_position = ble_on_position,
        .on_cmd = ble_on_cmd,
        .on_link = ble_on_link,
        .on_media_state = ble_on_media_state,
        .on_media_art = media_art_feed,
        .on_media_text = ble_on_media_text,
        .on_nav_state = ble_on_nav_state,
        .on_nav_dest = ble_on_nav_dest,
        .on_nav_limit = ble_on_nav_limit,
        .on_phone_info = ble_on_phone_info,
        .on_traffic = ble_on_traffic,
        .on_roads_chunk = ble_on_roads_chunk,
        .on_route_chunk = ble_on_route_chunk,
        .on_call_ring = ble_on_call_ring,
        .on_call_end = ble_on_call_end,
    };
    nav_ble_start(&cbs, settings_device_name());

    xTaskCreatePinnedToCore(gui_task, "gui", 10 * 1024, NULL, 5, NULL, 1);
}
