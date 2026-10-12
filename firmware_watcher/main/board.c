#include "board.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
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
    // also enables the Himax (AI), codec/PA and Grove rails; the nav firmware
    // uses none of them, so leaving them off saves power and keeps the Himax
    // off the SPI2 bus it shares with the microSD.
    s_exio_out |= (1u << EXIO_PWR_LCD) | (1u << EXIO_PWR_SD) | (1u << EXIO_PWR_BAT_ADC);
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

// Both input ports in one read (which also clears the expander's interrupt).
static bool exio_read(uint16_t *in)
{
    if (!s_exio_ready) return false;
    uint8_t reg = PCA9535_REG_INPUT0;
    uint8_t b[2] = {0};
    if (i2c_master_transmit_receive(s_exio, &reg, 1, b, 2, 100) != ESP_OK) return false;
    *in = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
    return true;
}

bool board_exio_get(int bit)
{
    uint16_t in;
    if (bit < 0 || bit > 15 || !exio_read(&in)) return false;
    return in & (1u << bit);
}

// The wheel, decoded in hardware by a pulse counter so no step is lost while
// the GUI task is busy pushing a frame. Both edges of both phases count, which
// is four counts per quadrature cycle; half a cycle is one step (a full cycle
// per step felt sluggish on the device).
#define KNOB_A 41
#define KNOB_B 42
#define KNOB_COUNTS_PER_STEP 2
#define KNOB_LIMIT 1000

static pcnt_unit_handle_t s_knob;
static int s_knob_seen;

void board_knob_init(void)
{
    const pcnt_unit_config_t unit = {
        .high_limit = KNOB_LIMIT,
        .low_limit = -KNOB_LIMIT,
        .flags.accum_count = 1,
    };
    const pcnt_glitch_filter_config_t filter = {.max_glitch_ns = 1000};
    const pcnt_chan_config_t cfg_a = {.edge_gpio_num = KNOB_A, .level_gpio_num = KNOB_B};
    const pcnt_chan_config_t cfg_b = {.edge_gpio_num = KNOB_B, .level_gpio_num = KNOB_A};
    pcnt_channel_handle_t a = NULL, b = NULL;
    pcnt_unit_handle_t u = NULL;
    if (pcnt_new_unit(&unit, &u) != ESP_OK || pcnt_unit_set_glitch_filter(u, &filter) != ESP_OK ||
        pcnt_new_channel(u, &cfg_a, &a) != ESP_OK || pcnt_new_channel(u, &cfg_b, &b) != ESP_OK) {
        ESP_LOGE(TAG, "knob: pulse counter unavailable");
        return;
    }
    pcnt_channel_set_edge_action(a, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(a, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    pcnt_channel_set_edge_action(b, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    pcnt_channel_set_level_action(b, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    // The limits are watch points so the driver carries the count across them.
    pcnt_unit_add_watch_point(u, KNOB_LIMIT);
    pcnt_unit_add_watch_point(u, -KNOB_LIMIT);
    gpio_set_pull_mode(KNOB_A, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(KNOB_B, GPIO_PULLUP_ONLY);
    pcnt_unit_enable(u);
    pcnt_unit_clear_count(u);
    pcnt_unit_start(u);
    s_knob = u;
}

int board_knob_steps(void)
{
    if (!s_knob) return 0;
    int count = 0;
    if (pcnt_unit_get_count(s_knob, &count) != ESP_OK) return 0;
    // Hand out whole steps only; a part-turned detent stays for next time.
    int steps = (count - s_knob_seen) / KNOB_COUNTS_PER_STEP;
    s_knob_seen += steps * KNOB_COUNTS_PER_STEP;
    return steps;
}

// Battery sense, copied from the vendor BSP (sensecap-watcher.c:377-432):
// GPIO3 = ADC1 channel 2 behind a 62k/20k divider, 2.5 dB attenuation, and the
// vendor's own voltage-to-percent curve.
#define BAT_ADC_CHAN ADC_CHANNEL_2
#define BAT_ADC_ATTEN ADC_ATTEN_DB_2_5
#define BAT_SAMPLES 10

bool board_battery_read(int *pct, bool *charging)
{
    static adc_oneshot_unit_handle_t adc;
    static adc_cali_handle_t cali;
    static bool tried;
    if (!tried) {
        tried = true;
        const adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
        const adc_oneshot_chan_cfg_t chan = {.bitwidth = ADC_BITWIDTH_DEFAULT, .atten = BAT_ADC_ATTEN};
        const adc_cali_curve_fitting_config_t cal = {
            .unit_id = ADC_UNIT_1,
            .chan = BAT_ADC_CHAN,
            .atten = BAT_ADC_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_oneshot_new_unit(&unit, &adc) != ESP_OK ||
            adc_oneshot_config_channel(adc, BAT_ADC_CHAN, &chan) != ESP_OK ||
            adc_cali_create_scheme_curve_fitting(&cal, &cali) != ESP_OK) {
            ESP_LOGE(TAG, "battery ADC unavailable");
            cali = NULL;
        }
    }
    // Not gated on EXIO_BAT_DET: on the bench unit it reads "absent" by the
    // vendor's polarity while the divider shows a healthy 4.19 V, and the
    // vendor's own firmware never consults it either.
    if (!cali || !s_exio_ready) return false;

    int sum = 0;
    for (int i = 0; i < BAT_SAMPLES; i++) {
        int raw = 0, mv = 0;
        adc_oneshot_read(adc, BAT_ADC_CHAN, &raw);
        adc_cali_raw_to_voltage(cali, raw, &mv);
        sum += mv;
    }
    int v = sum / BAT_SAMPLES * 82 / 20;  // battery mV
    int p = (-v * v + 9016 * v - 19189000) / 10000;
    *pct = p > 100 ? 100 : p < 0 ? 0 : p;
    // "On external power", not strictly "charging": VBUS-detect reads low with
    // USB plugged in. The charger's own CHRG/STDBY flags only tell charging
    // from full, which the percentage already shows.
    *charging = !board_exio_get(EXIO_VBUS_IN_DET);
    return true;
}

// Backlight, then the panel's QSPI bus: CS, clock, D0-D3 (see lcd.c).
static const gpio_num_t s_sleep_low_pins[] = {BL_GPIO, 45, 7, 9, 1, 14, 13};
#define SLEEP_LOW_PINS (sizeof(s_sleep_low_pins) / sizeof(s_sleep_low_pins[0]))

void board_sleep_pins_hold(void)
{
    // Take the backlight off its PWM channel first, idling low.
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    for (size_t i = 0; i < SLEEP_LOW_PINS; i++) {
        gpio_num_t pin = s_sleep_low_pins[i];
        gpio_reset_pin(pin);  // back to a plain GPIO, off the SPI/LEDC signal
        gpio_set_pull_mode(pin, GPIO_FLOATING);
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        gpio_set_level(pin, 0);
        gpio_hold_en(pin);
    }
    // Pins outside the RTC domain (the panel's CS) only keep their latch
    // through deep sleep with this set as well.
    gpio_deep_sleep_hold_en();
}

void board_sleep_pins_release(void)
{
    gpio_deep_sleep_hold_dis();
    for (size_t i = 0; i < SLEEP_LOW_PINS; i++) gpio_hold_dis(s_sleep_low_pins[i]);
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
    // the touch interrupt and the charger flags. A failed read is "not
    // pressed": a long press unpairs, and an I2C fault must not do that.
    uint16_t in;
    return exio_read(&in) && !(in & (1u << EXIO_KNOB_BTN));
}
