#include "board.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

// PCA9535 register pairs. Registers auto-increment across a multi-byte
// transfer, so one read of 2 bytes from REG_INPUT0 gives both input ports.
#define PCA9535_ADDR 0x21
#define PCA9535_REG_INPUT0 0x00
#define PCA9535_REG_OUTPUT0 0x02
#define PCA9535_REG_CONFIG0 0x06

// Port-1 halves of the direction masks (bits 8..15), derived from the vendor
// constants in docs-watcher/pinout.md: inputs 0x20FF, outputs 0xDF00.
#define CFG_PORT0 0xFF  // bits 0..7  all inputs
#define CFG_PORT1 0x20  // bit 13 (port1 bit 5) input, everything else output

#define BL_GPIO 8
#define BL_RES LEDC_TIMER_13_BIT
#define BL_MAX ((1 << 13) - 1)

static i2c_master_bus_handle_t s_bus;
static i2c_master_bus_handle_t s_touch_bus;
static i2c_master_dev_handle_t s_exio;

// Shadow of the output ports: never read-modify-write over I2C. Written as a
// pair (port 0 = low byte, port 1 = high byte) from bit 8 of the register map.
static uint16_t s_exio_out;

static bool s_exio_ready;

static esp_err_t exio_write_pair(uint8_t reg, uint8_t port0, uint8_t port1)
{
    uint8_t b[3] = {reg, port0, port1};
    return i2c_master_transmit(s_exio, b, sizeof(b), 100);
}

esp_err_t board_i2c_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = BOARD_I2C_NUM,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_bus);
}

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return s_bus;
}

esp_err_t board_touch_i2c_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = BOARD_TOUCH_I2C_NUM,
        .sda_io_num = BOARD_TOUCH_I2C_SDA,
        .scl_io_num = BOARD_TOUCH_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_touch_bus);
}

i2c_master_bus_handle_t board_touch_i2c_bus(void)
{
    return s_touch_bus;
}

esp_err_t board_exio_init(void)
{
    if (s_exio_ready) return ESP_OK;

    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCA9535_ADDR,
        .scl_speed_hz = BOARD_I2C_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(s_bus, &dev, &s_exio);
    if (err != ESP_OK) return err;

    // Order matters. The PCA9535 powers up with all pins as *inputs* pulled
    // weakly high, so a rail pin could read as "on" before we configure it —
    // and the vendor's own sequence sets the directions before clearing the
    // outputs. Zero the output registers first, then take the pins as outputs,
    // so no rail can be driven high by accident.
    err = exio_write_pair(PCA9535_REG_OUTPUT0, 0x00, 0x00);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCA9535 not responding: %s", esp_err_to_name(err));
        return err;
    }
    s_exio_out = 0;
    err = exio_write_pair(PCA9535_REG_CONFIG0, CFG_PORT0, CFG_PORT1);
    if (err != ESP_OK) return err;

    // Master rail first, on its own, then the peripherals. Same 100 ms / 50 ms
    // dwell as the vendor BSP (sensecap-watcher.c:145-181).
    s_exio_out |= (1u << EXIO_PWR_SYSTEM);
    err = exio_write_pair(PCA9535_REG_OUTPUT0, s_exio_out & 0xFF, s_exio_out >> 8);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(100));

    // Only the rails this firmware uses are raised. The vendor's start-up mask
    // also enables the Himax (AI), codec/PA, Grove and battery-divider rails;
    // the nav firmware uses none of them, so leaving them off saves power and
    // keeps the Himax off the SPI2 bus it shares with the microSD.
    s_exio_out |= (1u << EXIO_PWR_LCD) | (1u << EXIO_PWR_SD);
    err = exio_write_pair(PCA9535_REG_OUTPUT0, s_exio_out & 0xFF, s_exio_out >> 8);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(50));

    s_exio_ready = true;
    ESP_LOGW(TAG, "rails up: LCD | SD | SYSTEM (AI/codec/Grove off)");
    return ESP_OK;
}

esp_err_t board_exio_set(int bit, bool level)
{
    if (bit < 0 || bit > 15) return ESP_ERR_INVALID_ARG;
    if (level) {
        s_exio_out |= (1u << bit);
    } else {
        s_exio_out &= ~(1u << bit);
    }
    // Write both ports: the two calls that set individual bits (rails, and the
    // boot-button path below) are rare enough that the extra byte is free.
    return exio_write_pair(PCA9535_REG_OUTPUT0, s_exio_out & 0xFF, s_exio_out >> 8);
}

bool board_exio_get(int bit)
{
    if (!s_exio_ready || bit < 0 || bit > 15) return false;
    uint8_t reg = PCA9535_REG_INPUT0;
    uint8_t in[2] = {0};
    if (i2c_master_transmit_receive(s_exio, &reg, 1, in, 2, 100) != ESP_OK) return false;
    return ((uint16_t)in[0] | ((uint16_t)in[1] << 8)) & (1u << bit);
}

void board_power_rails_off(void)
{
    if (!s_exio_ready) return;
    // Mirrors the vendor's sleep mask (sensecap-watcher.c:334). EXIO_PWR_SYSTEM
    // stays up: clearing it would cut power to the whole device rather than
    // sleep it.
    s_exio_out &= ~((1u << EXIO_PWR_SD) | (1u << EXIO_PWR_LCD) | (1u << EXIO_PWR_AI) |
                    (1u << EXIO_PWR_CODEC_PA) | (1u << EXIO_PWR_GROVE) | (1u << EXIO_PWR_BAT_ADC));
    exio_write_pair(PCA9535_REG_OUTPUT0, s_exio_out & 0xFF, s_exio_out >> 8);
}

void board_backlight_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BL_RES,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&t);
    ledc_channel_config_t c = {
        .gpio_num = BL_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    ledc_channel_config(&c);
}

void board_backlight_set(uint8_t pct)
{
    if (pct > 100) pct = 100;
    uint32_t duty = (uint32_t)pct * BL_MAX / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void board_boot_button_init(void)
{
    // Nothing to configure: the wheel switch is an expander input, set up by
    // board_exio_init(). Kept so the call site reads the same as v2's.
}

bool board_boot_button_pressed(void)
{
    // Active low, and only on the *button* bit — the same register also carries
    // the touch interrupt and the charger flags.
    return !board_exio_get(EXIO_KNOB_BTN);
}
