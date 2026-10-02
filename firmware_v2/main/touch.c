#include "touch.h"
#include "board.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "touch";

#define GT911_ADDR 0x5D
#define GT911_INT_GPIO 16
#define REG_STATUS 0x814E
#define REG_POINTS 0x814F
#define REG_PRODUCT_ID 0x8140

// If the controller stops reporting while a finger is down (lost release
// frame), declare release after this long.
#define STALE_US 150000

static i2c_master_dev_handle_t s_dev;
static touch_sample_t s_sample;
static int64_t s_last_update_us;

static esp_err_t rd(uint16_t reg, uint8_t *buf, size_t len)
{
    uint8_t a[2] = {reg >> 8, reg & 0xFF};
    return i2c_master_transmit_receive(s_dev, a, 2, buf, len, 50);
}

static esp_err_t wr8(uint16_t reg, uint8_t v)
{
    uint8_t a[3] = {reg >> 8, reg & 0xFF, v};
    return i2c_master_transmit(s_dev, a, 3, 50);
}

esp_err_t touch_init(void)
{
    // Reset with INT held low selects I2C address 0x5D.
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << GT911_INT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(GT911_INT_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    board_exio_set(EXIO_TOUCH_RST, false);
    vTaskDelay(pdMS_TO_TICKS(20));
    board_exio_set(EXIO_TOUCH_RST, true);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_direction(GT911_INT_GPIO, GPIO_MODE_INPUT);
    vTaskDelay(pdMS_TO_TICKS(60));

    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = GT911_ADDR,
        .scl_speed_hz = BOARD_I2C_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(board_i2c_bus(), &dev, &s_dev);
    if (err != ESP_OK) return err;

    uint8_t id[4] = {0};
    err = rd(REG_PRODUCT_ID, id, 4);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GT911 not responding: %s", esp_err_to_name(err));
        return err;
    }
    wr8(REG_STATUS, 0);
    return ESP_OK;
}

void touch_poll(void)
{
    if (!s_dev) return;
    int64_t now = esp_timer_get_time();
    uint8_t st = 0;
    if (rd(REG_STATUS, &st, 1) != ESP_OK) {
        if (s_sample.n && now - s_last_update_us > STALE_US) s_sample.n = 0;
        return;
    }
    if (!(st & 0x80)) {
        if (s_sample.n && now - s_last_update_us > STALE_US) s_sample.n = 0;
        return;
    }
    uint8_t n = st & 0x0F;
    if (n > 5) n = 0;
    touch_sample_t s = {0};
    if (n > 0) {
        uint8_t use = n > 2 ? 2 : n;
        uint8_t buf[16];
        if (rd(REG_POINTS, buf, use * 8) == ESP_OK) {
            for (int i = 0; i < use; i++) {
                int x = buf[i * 8 + 1] | (buf[i * 8 + 2] << 8);
                int y = buf[i * 8 + 3] | (buf[i * 8 + 4] << 8);
                if (x >= BOARD_LCD_W) x = BOARD_LCD_W - 1;
                if (y >= BOARD_LCD_H) y = BOARD_LCD_H - 1;
                s.x[i] = x;
                s.y[i] = y;
            }
            s.n = use;
        }
    }
    wr8(REG_STATUS, 0);
    s_sample = s;
    s_last_update_us = now;
}

const touch_sample_t *touch_get(void)
{
    return &s_sample;
}
