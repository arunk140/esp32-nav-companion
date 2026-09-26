#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"

#include "settings.h"
#include "display/backlight.h"
#include "display/lvgl9_port.h"
#include "ui/pairing_screen.h"
#include "map/map_view.h"
#include "ble/ble_service.h"

// Vendor drivers (firmware/main/drivers/, see firmware/README.md).
#include "I2C_Driver/I2C_Driver.h"
#include "EXIO/TCA9554PWR.h"
#include "LCD_Driver/ST7701S.h"
#include "Touch_Driver/GT911.h"
#include "SD_Card/SD_MMC.h"

static const char *TAG = "nav";

// Init order is load-bearing: I2C -> EXIO -> LCD -> Touch -> SD -> LVGL.
// LCD before SD is mandatory (shared GPIO1/2). EXIO before LCD/Touch/SD
// (CS/RST on expander). See docs/waveshare-2.8c/02-pinout.md.

static void driver_init(void)
{
    I2C_Init();
    vTaskDelay(pdMS_TO_TICKS(120));
    EXIO_Init();
    LCD_Init();
    Touch_Init();
    SD_Init();  // after LCD, always
}

// BLE callbacks fire in the NimBLE host task — never touch LVGL here.
// They stash state; gui_task (below) consumes it in LVGL context.
static void show_map(void *arg);  // defined below
static void show_pairing(void);   // defined below
static volatile bool s_recenter_req;
static volatile bool s_zoom_req_new;
static volatile uint8_t s_zoom_req;
static volatile bool s_show_pairing_req;
static volatile bool s_show_map_req;
static volatile bool s_save_pos_req;    // persist last fix (GUI task does NVS)
static volatile bool s_bright_save_req; // persist brightness (GUI task does NVS)
static volatile uint8_t s_bright_save;
static volatile bool s_bonded_req;      // persist bond flag (GUI task does NVS)
static volatile bool s_bonded_val;
static volatile bool s_link_up;
static volatile bool s_pos_new;
static int32_t s_pos_lat, s_pos_lon;
static volatile uint32_t s_pos_ms;
static volatile uint32_t s_pos_age_ms;
static volatile bool s_got_pos;  // at least one fix stashed

static void on_position(int32_t lat_e7, int32_t lon_e7, uint16_t speed_cms,
                        uint16_t cog_cdeg, uint8_t sats, uint8_t fix,
                        uint32_t ts_ms)
{
    (void)speed_cms;
    (void)cog_cdeg;
    (void)sats;
    (void)fix;
    // Defensive: never let a null-island (0,0) or out-of-range fix move the
    // map. Fused providers emit (0,0) before first fix; rendering it
    // recenters to empty ocean and looks like a blank/broken map.
    if ((lat_e7 == 0 && lon_e7 == 0) || lat_e7 > 900000000 ||
        lat_e7 < -900000000 || lon_e7 > 1800000000 || lon_e7 < -1800000000) {
        ESP_LOGW(TAG, "ignoring bogus fix %ld,%ld", (long)lat_e7, (long)lon_e7);
        return;
    }
    s_pos_lat = lat_e7;
    s_pos_lon = lon_e7;
    s_pos_ms = ts_ms;
    s_pos_new = true;
    s_got_pos = true;
    // Link-quality signal: log gaps >2 s (stale timeout is 12 s).
    static uint32_t last_arr;
    uint32_t now_arr = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (last_arr && now_arr - last_arr > 2000) {
        ESP_LOGI(TAG, "position gap %lums", (unsigned long)(now_arr - last_arr));
    }
    last_arr = now_arr;
    // Persist throttled (60 s): flash wears under a 1 Hz stream otherwise.
    // Survives reboot as the gray marker. Flagged for the GUI task: NVS
    // flash writes must never run in the NimBLE task — BLE + flash write
    // contention wedges the controller (Core 0 interrupt WDT).
    static uint32_t last_save;
    if (!last_save || now_arr - last_save > 60000) {
        last_save = now_arr;
        s_save_pos_req = true;
    }
}

static void on_cmd(uint8_t code, const uint8_t *args, size_t len)
{
    switch (code) {
    case NAV_CMD_RECENTER:
        // Handled in gui context via flag: reuse position path is overkill;
        // recenter-to-default is a map call, so stash a request.
        s_recenter_req = true;
        break;
    case NAV_CMD_SET_ZOOM:
        if (len >= 1) {
            s_zoom_req = args[0];
            s_zoom_req_new = true;
        }
        break;
    case NAV_CMD_PING:
        ESP_LOGI(TAG, "ping");
        break;
    case NAV_CMD_UNPAIR:
        ESP_LOGI(TAG, "remote unpair");
        s_bonded_req = true;
        s_bonded_val = false;
        s_show_pairing_req = true;
        break;
    case NAV_CMD_SET_BRIGHTNESS:
        if (len >= 1) {
            // Applied now (LEDC is thread-safe); persisted by the GUI task
            // (NVS must not run in the NimBLE task — see on_position).
            display_set_brightness(args[0]);
            s_bright_save = args[0];
            s_bright_save_req = true;
        }
        break;
    case NAV_CMD_PAIR: {
        // 6 ASCII digits must match the token on the QR screen.
        // NOTE: this runs in the NimBLE task — never touch LVGL or NVS here.
        // Stash the request; gui_task performs the screen switch + persist.
        char tok[7] = {0};
        if (len >= 6) {
            memcpy(tok, args, 6);
            char expect[7];
            snprintf(expect, sizeof(expect), "%06lu",
                     (unsigned long)settings_get_token());
            if (!strcmp(tok, expect)) {
                ESP_LOGI(TAG, "pairing verified");
                s_bonded_req = true;
                s_bonded_val = true;
                s_show_map_req = true;
            } else {
                ESP_LOGW(TAG, "pairing token mismatch");
            }
        }
        break;
    }
    default:
        break;
    }
}

static void on_connect(void)
{
    s_link_up = true;
}

static void on_disconnect(void)
{
    s_link_up = false;
}

// LVGL render + touch handling needs a deep call stack; the main task's
// default ~3.5 KB stack overflows. Run it on a dedicated 12 KB task.
static void boot_button_poll(void)
{
    // BOOT (GPIO0, active low) held 5 s -> factory unpair + pairing screen.
    // Escape hatch for orphaned bonds (phone unpaired, device still paired).
    static uint32_t low_ms;
    static bool fired;
    if (gpio_get_level(GPIO_NUM_0) == 0) {
        if (!low_ms) low_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (!fired && xTaskGetTickCount() * portTICK_PERIOD_MS - low_ms > 5000) {
            fired = true;
            ESP_LOGI(TAG, "BOOT long-press: factory unpair");
            s_bonded_req = true;
            s_bonded_val = false;
            show_pairing();
        }
    } else {
        low_ms = 0;
        fired = false;
    }
}

static void gui_task(void *arg)
{
    (void)arg;
    static bool stale_shown;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5));
        boot_button_poll();
        // Consume BLE stash in LVGL context.
        if (s_pos_new) {
            s_pos_new = false;
            stale_shown = false;
            map_view_set_position(s_pos_lat / 1e7, s_pos_lon / 1e7, true);
            s_pos_age_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        }
        if (s_recenter_req) {
            s_recenter_req = false;
            map_view_recenter_default();
        }
        if (s_zoom_req_new) {
            s_zoom_req_new = false;
            map_view_set_zoom(s_zoom_req);
        }
        if (s_show_pairing_req) {
            s_show_pairing_req = false;
            stale_shown = false;
            show_pairing();
        }
        if (s_show_map_req) {
            s_show_map_req = false;
            stale_shown = false;
            show_map(NULL);
        }
        // Deferred NVS writes (flagged by NimBLE-task callbacks above).
        if (s_save_pos_req) {
            s_save_pos_req = false;
            settings_set_last_pos(s_pos_lat, s_pos_lon);
        }
        if (s_bright_save_req) {
            s_bright_save_req = false;
            settings_set_brightness(s_bright_save);
        }
        if (s_bonded_req) {
            s_bonded_req = false;
            nav_ble_set_bonded(s_bonded_val);
        }
        // Stale link: no POSITION for 12 s. The phone batches fixes ~10.5 s
        // apart under Doze, so the timeout must cover a full batch period;
        // otherwise the marker flickers blue/gray on a healthy link.
        // (Tighten once delivery is proven at 1 Hz.)
        if (!stale_shown && s_got_pos && s_link_up &&
            (xTaskGetTickCount() * portTICK_PERIOD_MS - s_pos_age_ms) > 12000) {
            stale_shown = true;
            map_view_set_position(s_pos_lat / 1e7, s_pos_lon / 1e7, false);
        }
        lvgl9_tick_loop();
        map_view_poll();  // no-op until map screen exists
    }
}

static void show_pairing(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    map_view_destroy();
    pairing_screen_create(scr, show_map, NULL);
}

static void show_map(void *arg)
{
    (void)arg;
    // Re-entry guard: PAIR/token can arrive while the map is already live
    // (reconnects, duplicate writes). Re-creating would leak the tiles
    // handle and churn LVGL objects mid-render.
    extern bool map_view_is_live(void);
    if (map_view_is_live()) {
        return;
    }
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    map_view_create(scr);
}

void app_main(void)
{
    settings_init();
    driver_init();
    backlight_init();  // restores NVS brightness
    lvgl9_init();

    static const nav_ble_cbs_t ble_cbs = {
        .on_position = on_position,
        .on_cmd = on_cmd,
        .on_connect = on_connect,
        .on_disconnect = on_disconnect,
    };
    nav_ble_start(&ble_cbs);

    // BOOT button (GPIO0) as plain input for the long-press escape hatch.
    gpio_config_t boot_cfg = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&boot_cfg);

    if (!settings_get_paired()) {
        show_pairing();
    } else {
        show_map(NULL);
    }

    ESP_LOGI(TAG, "M1 ready");
    xTaskCreatePinnedToCore(gui_task, "gui", 12288, NULL, 4, NULL, 1);
    vTaskDelete(NULL);  // free the small main-task stack
}
