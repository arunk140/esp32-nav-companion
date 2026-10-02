#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"

#include "settings.h"
#include "display/backlight.h"
#include "display/panel.h"
#include "input/touch.h"
#include "gfx/gfx.h"
#include "ui/ui.h"
#include "map/mapview.h"
#include "ble/ble_service.h"

// Vendor board drivers (see docs/waveshare-2.8c/02-pinout.md for the map).
#include "I2C_Driver/I2C_Driver.h"
#include "EXIO/TCA9554PWR.h"
#include "LCD_Driver/ST7701S.h"
#include "Touch_Driver/GT911.h"
#include "SD_Card/SD_MMC.h"

static const char *TAG = "nav";

// Init order is load-bearing: I2C -> EXIO -> LCD -> Touch -> SD. EXIO gates
// the reset/CS lines of everything else, and LCD must precede SD because the
// two share GPIO1/2. Any LCD re-init must be followed by SD_Init().
static void driver_init(void)
{
    I2C_Init();
    vTaskDelay(pdMS_TO_TICKS(120));
    EXIO_Init();
    LCD_Init();
    Touch_Init();
    SD_Init();
}

// --- BLE hand-off ----------------------------------------------------------
// NimBLE callbacks run in the BLE host task. They must not touch LVGL (there
// is none now) and must not write NVS: flash writes contend with the BLE
// controller and wedge the host task into an interrupt-WDT trip. So they only
// set flags, and the GUI task does the work.
static volatile bool s_recenter_req;
static volatile bool s_zoom_req_new;
static volatile uint8_t s_zoom_req;
static volatile bool s_show_pairing_req;
static volatile bool s_show_map_req;
static volatile bool s_save_pos_req;
static volatile bool s_bright_save_req;
static volatile uint8_t s_bright_save;
static volatile bool s_bonded_req;
static volatile bool s_bonded_val;
static volatile bool s_link_up;
static volatile bool s_pos_new;
static volatile int32_t s_pos_lat, s_pos_lon;  // volatile: cross-task payload
static volatile int32_t s_pos_speed;
static volatile uint32_t s_pos_age_ms;
static volatile bool s_got_pos;

static void on_position(int32_t lat_e7, int32_t lon_e7, uint16_t speed_cms,
                        uint16_t cog_cdeg, uint8_t sats, uint8_t fix,
                        uint32_t ts_ms)
{
    (void)cog_cdeg;
    (void)sats;
    (void)fix;
    (void)ts_ms;
    // Never let a null-island or out-of-range fix move the map: fused
    // providers emit (0,0) before the first fix, and rendering it recentres
    // to empty ocean.
    if ((lat_e7 == 0 && lon_e7 == 0) || lat_e7 > 900000000 ||
        lat_e7 < -900000000 || lon_e7 > 1800000000 || lon_e7 < -1800000000) {
        return;
    }
    s_pos_lat = lat_e7;
    s_pos_lon = lon_e7;
    s_pos_speed = speed_cms;
    s_pos_new = true;
    s_got_pos = true;
    // Persist at most once a minute: a 1 Hz stream would wear the flash out.
    static uint32_t last_save;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (!last_save || now - last_save > 60000) {
        last_save = now;
        s_save_pos_req = true;
    }
}

static void on_cmd(uint8_t code, const uint8_t *args, size_t len)
{
    switch (code) {
    case NAV_CMD_RECENTER:
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
        s_bonded_req = true;
        s_bonded_val = false;
        s_show_pairing_req = true;
        break;
    case NAV_CMD_SET_BRIGHTNESS:
        if (len >= 1) {
            display_set_brightness(args[0]);  // LEDC is thread-safe
            s_bright_save = args[0];
            s_bright_save_req = true;
        }
        break;
    case NAV_CMD_PAIR: {
        // 6 ASCII digits must match the token on the QR screen.
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

// BOOT (GPIO0) held 5 s: factory unpair. Escape hatch for an orphaned bond
// (phone unpaired, device still thinks it is paired).
static void boot_button_poll(void)
{
    static uint32_t low_ms;
    static bool fired;
    if (gpio_get_level(GPIO_NUM_0) == 0) {
        if (!low_ms) low_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (!fired && xTaskGetTickCount() * portTICK_PERIOD_MS - low_ms > 5000) {
            fired = true;
            ESP_LOGI(TAG, "BOOT long-press: factory unpair");
            s_bonded_req = true;
            s_bonded_val = false;
            ui_show_pairing();
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
        // One tick, NOT pdMS_TO_TICKS(5): FREERTOS_HZ is 100, so a 5 ms request
        // truncates to ZERO ticks and this becomes a free-running spin that
        // starves IDLE1 and trips the task watchdog. 100 Hz is ample for the
        // ease and the pan.
        vTaskDelay(1);
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        boot_button_poll();

        if (s_pos_new) {
            s_pos_new = false;
            stale_shown = false;
            mapview_set_position(s_pos_lat / 1e7, s_pos_lon / 1e7, true);
            mapview_set_speed(s_pos_speed);
            s_pos_age_ms = now;
        }
        if (s_recenter_req) {
            s_recenter_req = false;
            mapview_recenter();
        }
        if (s_zoom_req_new) {
            s_zoom_req_new = false;
            mapview_set_zoom(s_zoom_req);
        }
        if (s_show_pairing_req) {
            s_show_pairing_req = false;
            stale_shown = false;
            ui_show_pairing();
        }
        if (s_show_map_req) {
            s_show_map_req = false;
            stale_shown = false;
            ui_show_map();
        }
        // Deferred NVS writes (flagged from the BLE task above).
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
        // A link that has gone quiet for 12 s: the phone batches ~10.5 s apart
        // under Doze, so the timeout has to cover a full batch or the marker
        // flickers blue/gray on a healthy link.
        if (!stale_shown && s_got_pos && s_link_up && (now - s_pos_age_ms) > 12000) {
            stale_shown = true;
            mapview_set_position(s_pos_lat / 1e7, s_pos_lon / 1e7, false);
        }

        ui_set_linked(s_link_up);
        ui_frame(now);
    }
}

void app_main(void)
{
    settings_init();
    driver_init();
    backlight_init();  // restores NVS brightness
    gfx_init();
    ui_init();

    static const nav_ble_cbs_t ble_cbs = {
        .on_position = on_position,
        .on_cmd = on_cmd,
        .on_connect = on_connect,
        .on_disconnect = on_disconnect,
    };
    nav_ble_start(&ble_cbs);

    gpio_config_t boot_cfg = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&boot_cfg);

    if (settings_get_paired()) ui_show_map();
    else ui_show_pairing();

    ESP_LOGI(TAG, "v2 ready");
    // 12 KB: rendering and the gesture path both recurse a little, and the
    // vendor LCD init is the deepest thing on the board.
    xTaskCreatePinnedToCore(gui_task, "gui", 12288, NULL, 4, NULL, 1);
    vTaskDelete(NULL);
}
