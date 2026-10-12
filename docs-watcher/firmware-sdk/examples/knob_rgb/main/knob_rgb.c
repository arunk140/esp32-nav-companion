
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_log.h"
#include <string.h>

#include "sensecap-watcher.h"

#include "knob_rgb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static const char *TAG = "Knob_RGB";

static led_strip_handle_t s_rgb = 0;
static button_handle_t s_btn = 0;
static knob_handle_t s_knob = 0;
bool led_on = false;

/******************************************************************************************
 * RGB LED
 ******************************************************************************************/

static esp_err_t example_rgb_init()
{
    led_strip_config_t example_strip_config = {
        .strip_gpio_num = BSP_RGB_CTRL,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t example_rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&example_strip_config, &example_rmt_config, &s_rgb));
    led_strip_set_pixel(s_rgb, 0, 0x00, 0x00, 0x00);
    led_strip_refresh(s_rgb);

    return ESP_OK;
}

static esp_err_t example_rgb_set(uint8_t r, uint8_t g, uint8_t b)
{
    esp_err_t ret = ESP_OK;
    uint32_t index = 0;

    ret |= led_strip_set_pixel(s_rgb, index, r, g, b);
    ret |= led_strip_refresh(s_rgb);
    return ret;
}

static void example_rgb_next_color(bool reverse)
{
    static uint8_t step = 4;
    static uint8_t target_color[3] = { 0, 255, 0 };
    static uint8_t current_color[3] = { 255, 0, 0 };

    for (int i = 0; i < 3; i++)
    {
        int pos = reverse ? ((i - 1 + 3) % 3) : ((i + 1) % 3);
        if (target_color[i] == 255 && current_color[i] == 255)
        {
            target_color[i] = 0;
            target_color[pos] = 255;
            break;
        }
    }

    for (int i = 0; i < 3; i++)
    {
        if (current_color[i] < target_color[i])
        {
            current_color[i] = current_color[i] > 255 - step ? 255 : current_color[i] + step;
        }
        else if (current_color[i] > target_color[i])
        {
            current_color[i] = current_color[i] < step ? 0 : current_color[i] - step;
        }
    }

    ESP_ERROR_CHECK(example_rgb_set(current_color[0] / 2, current_color[1] / 2, current_color[2] / 2));
}

/******************************************************************************************
 * Button
 ******************************************************************************************/

static void example_btn_press_down_cb(void *arg, void *data)
{
    ESP_LOGI(TAG, "BTN: BUTTON_PRESS_DOWN");
    if (led_on)
    {
        example_rgb_next_color(true);
        led_on = false;
    }
    else
    {
        led_strip_clear(s_rgb);
        led_on = true;
    }
}

static void example_btn_press_up_cb(void *arg, void *data)
{
    ESP_LOGI(TAG, "BTN: BUTTON_PRESS_UP[%d]", iot_button_get_ticks_time((button_handle_t)arg));
}

static esp_err_t example_btn_init(void)
{
    // button_config_t cfg = {
    //     .type = BUTTON_TYPE_GPIO,
    //     .long_press_time = 1000,
    //     .short_press_time = 200,
    //     .gpio_button_config = {
    //         .gpio_num  = BSP_BTN_IO,
    //         .active_level = 0,
    //     },
    // };
    // s_btn = iot_button_create(&cfg);
    // if (NULL == s_btn)
    // {
    //     ESP_LOGE(TAG, "button create failed");
    //     return ESP_FAIL;
    // }
    // iot_button_register_cb(s_btn, BUTTON_PRESS_DOWN, example_btn_press_down_cb, NULL);
    // iot_button_register_cb(s_btn, BUTTON_PRESS_UP, example_btn_press_up_cb, NULL);
    return ESP_OK;
}

/******************************************************************************************
 * Knob
 ******************************************************************************************/

static void example_knob_right_cb(void *arg, void *data)
{
    ESP_LOGI(TAG, "KONB: KONB_RIGHT,count_value:%d", iot_knob_get_count_value((button_handle_t)arg));
    example_rgb_next_color(true);
}

static void example_knob_left_cb(void *arg, void *data)
{
    ESP_LOGI(TAG, "KONB: KONB_LEFT,count_value:%d", iot_knob_get_count_value((button_handle_t)arg));
    example_rgb_next_color(false);
}

static esp_err_t example_knob_init(void)
{
    knob_config_t cfg = {
        .default_direction = 0,
        .gpio_encoder_a = BSP_KNOB_A,
        .gpio_encoder_b = BSP_KNOB_B,
    };
    s_knob = iot_knob_create(&cfg);
    if (NULL == s_knob)
    {
        ESP_LOGE(TAG, "knob create failed");
        return ESP_FAIL;
    }

    iot_knob_register_cb(s_knob, KNOB_LEFT, example_knob_left_cb, NULL);
    iot_knob_register_cb(s_knob, KNOB_RIGHT, example_knob_right_cb, NULL);
    return ESP_OK;
}

void app_main(void)
{
    ESP_ERROR_CHECK(example_rgb_init());
    // ESP_ERROR_CHECK(example_btn_init());
    ESP_ERROR_CHECK(example_knob_init());

    ESP_LOGI(TAG, "Start blinking LED strip");
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
