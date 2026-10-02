#include "board.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

#define TCA9554_ADDR 0x20
#define TCA_REG_OUTPUT 0x01
#define TCA_REG_CONFIG 0x03

#define BL_GPIO 6
#define BL_RES LEDC_TIMER_13_BIT
#define BL_MAX ((1 << 13) - 1)

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_exio;
// Shadow of the output register: never read-modify-write over I2C.
static uint8_t s_exio_out;

esp_err_t board_i2c_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
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

static esp_err_t exio_write(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(s_exio, b, 2, 100);
}

esp_err_t board_exio_init(void)
{
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA9554_ADDR,
        .scl_speed_hz = BOARD_I2C_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(s_bus, &dev, &s_exio);
    if (err != ESP_OK) return err;
    // Output levels first (power-on default is all-high, which would sound
    // the buzzer the moment the pins turn into outputs): everything high
    // (resets released, CS deselected, SD D3 high), buzzer off.
    s_exio_out = 0xFF & ~(1u << EXIO_BUZZER);
    err = exio_write(TCA_REG_OUTPUT, s_exio_out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TCA9554 not responding: %s", esp_err_to_name(err));
        return err;
    }
    // IMU interrupt lines are inputs; everything else outputs.
    return exio_write(TCA_REG_CONFIG, (1u << EXIO_IMU_INT1) | (1u << EXIO_IMU_INT2));
}

esp_err_t board_exio_set(int bit, bool level)
{
    if (level) {
        s_exio_out |= (1u << bit);
    } else {
        s_exio_out &= ~(1u << bit);
    }
    return exio_write(TCA_REG_OUTPUT, s_exio_out);
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
    gpio_config_t c = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&c);
}

bool board_boot_button_pressed(void)
{
    return gpio_get_level(GPIO_NUM_0) == 0;
}
