#include "touch.h"
#include "board.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_spd2010.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "touch";

// If the controller stops reporting while a finger is down (a lost release
// frame, or a run of I2C errors), declare release after this long. Clearing the
// sample on the first failure instead would strobe press/release for a frame
// and jitter every drag.
#define STALE_US 150000

static esp_lcd_touch_handle_t s_tp;
static touch_sample_t s_sample;
static int64_t s_last_update_us;

esp_err_t touch_init(void)
{
    // Touch lives on its own I2C bus (I2C1) on this board — the expander and
    // RTC are on I2C0. The vendor wires the panel's interrupt to the expander
    // (bit 5), where it exists only to wake the chip from deep sleep; polling
    // is what reads it.
    esp_err_t err = board_touch_i2c_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c1: %s", esp_err_to_name(err));
        return err;
    }

    const esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_SPD2010_CONFIG();
    esp_lcd_panel_io_handle_t io = NULL;
    err = esp_lcd_new_panel_io_i2c(board_touch_i2c_bus(), &io_config, &io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel io: %s", esp_err_to_name(err));
        return err;
    }

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_W,
        .y_max = BOARD_LCD_H,
        // Shared with the panel and not connected to a GPIO: the LCD power
        // rail (raised in board_exio_init) is what resets both.
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels.reset = 0,
        .levels.interrupt = 0,
        .flags.swap_xy = 0,
        .flags.mirror_x = 0,
        .flags.mirror_y = 0,
    };
    err = esp_lcd_touch_new_i2c_spd2010(io, &tp_cfg, &s_tp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPD2010 touch not responding: %s", esp_err_to_name(err));
        return err;
    }

    // The vendor reads once to settle the controller right after init
    // (sensecap-watcher.c:756-758).
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_lcd_touch_read_data(s_tp);
    return ESP_OK;
}

void touch_poll(void)
{
    if (!s_tp) return;
    int64_t now = esp_timer_get_time();

    if (esp_lcd_touch_read_data(s_tp) != ESP_OK) {
        if (s_sample.n && now - s_last_update_us > STALE_US) s_sample.n = 0;
        return;
    }

    // Never assume the driver wrote point_cnt: esp_lcd_touch_get_data returns
    // early (ESP_OK) when nothing is touched, and only guarantees the count on
    // a hit.
    uint8_t n = 0;
    esp_lcd_touch_point_data_t pts[2];
    esp_lcd_touch_get_data(s_tp, pts, &n, 2);
    if (n > 2) n = 2;

    // The SPD2010 only has points to give when it has raised a new report. We
    // poll faster than it reports, so most reads while a finger is down come
    // back OK with nothing in them - that is "no news", not a release, and
    // treating it as one strobes press/release and kills every drag. Hold the
    // last sample instead; the stale timeout covers a lost release.
    if (n == 0) {
        if (s_sample.n && now - s_last_update_us > STALE_US) s_sample.n = 0;
        return;
    }

    // The release itself arrives as a report whose point has zero weight, at
    // the position the finger left from. Drop those: a report with nothing
    // else in it is the release.
    touch_sample_t s = {0};
    for (int i = 0; i < n; i++) {
        if (pts[i].strength == 0) continue;
        int x = pts[i].x, y = pts[i].y;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= BOARD_LCD_W) x = BOARD_LCD_W - 1;
        if (y >= BOARD_LCD_H) y = BOARD_LCD_H - 1;
        s.x[s.n] = (int16_t)x;
        s.y[s.n] = (int16_t)y;
        s.n++;
    }

    s_sample = s;
    s_last_update_us = now;
}

const touch_sample_t *touch_get(void)
{
    return &s_sample;
}
