#include "display/lvgl9_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "LCD_Driver/ST7701S.h"   // panel_handle, EXAMPLE_LCD_* (via drivers/ include path)
#include "Touch_Driver/GT911.h"   // tp

static const char *TAG = "lvgl9";

#define HOR_RES 480
#define VER_RES 480
#define DRAW_LINES 40  // partial buffer height

static lv_display_t *s_disp;
static uint8_t *s_buf1;
static uint8_t *s_buf2;
static esp_timer_handle_t s_tick_timer;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    // FPS accounting: count flushes per wall second.
    static uint32_t n_flush, t_flush;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    n_flush++;
    if (now - t_flush > 1000) {
        ESP_LOGI(TAG, "flush %lu/s", (unsigned long)n_flush);
        n_flush = 0;
        t_flush = now;
    }
    esp_lcd_panel_draw_bitmap(panel_handle,
                              area->x1, area->y1, area->x2 + 1, area->y2 + 1,
                              px_map);
    lv_display_flush_ready(disp);
}

static void tick_cb(void *arg)
{
    lv_tick_inc(2);  // timer fires every 2 ms
}

// Last touch sample (up to 2 points), updated by touch_read_cb on every
// indev read. Consumers (pinch) MUST read this — never call
// esp_lcd_touch_read_data/get_coordinates directly, because get_coordinates
// invalidates the sample (points=0) and a second reader starves LVGL.
static uint16_t s_sx[2];
static uint16_t s_sy[2];
static uint8_t s_scnt;

void touch_last_sample(uint16_t *x, uint16_t *y, uint8_t *cnt)
{
    if (x) { x[0] = s_sx[0]; x[1] = s_sx[1]; }
    if (y) { y[0] = s_sy[0]; y[1] = s_sy[1]; }
    if (cnt) *cnt = s_scnt;
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint16_t x[2] = {0, 0}, y[2] = {0, 0};
    uint8_t cnt = 0;
    esp_lcd_touch_read_data(tp);
    bool pressed = esp_lcd_touch_get_coordinates(tp, x, y, NULL, &cnt, 2);
    s_sx[0] = x[0]; s_sx[1] = x[1];
    s_sy[0] = y[0]; s_sy[1] = y[1];
    s_scnt = pressed ? cnt : 0;
    if (pressed && cnt > 0) {
        // Reject touches in the hidden corners (round bezel covers them).
        int dx = (int)x[0] - HOR_RES / 2, dy = (int)y[0] - VER_RES / 2;
        if (dx * dx + dy * dy > 225 * 225) {
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
        data->point.x = x[0];
        data->point.y = y[0];
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void lvgl9_init(void)
{
    lv_init();

    size_t px = HOR_RES * DRAW_LINES;
    s_buf1 = heap_caps_malloc(px * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_buf2 = heap_caps_malloc(px * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(s_buf1 && s_buf2);

    s_disp = lv_display_create(HOR_RES, VER_RES);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_buffers(s_disp, s_buf1, s_buf2, px * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    lv_indev_set_display(indev, s_disp);

    const esp_timer_create_args_t args = {
        .callback = tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_create(&args, &s_tick_timer);
    esp_timer_start_periodic(s_tick_timer, 2000);
    ESP_LOGI(TAG, "LVGL9 ready (%dx%d, partial x%d)", HOR_RES, VER_RES, DRAW_LINES);
}

void lvgl9_tick_loop(void)
{
    // Handler-duration accounting: max lv_timer_handler() time per second.
    // If this approaches the loop period, the GUI is render-bound and input
    // queues behind drawing (feels like lag).
    int64_t t0 = esp_timer_get_time();
    lv_timer_handler();
    int64_t dt = esp_timer_get_time() - t0;
    static int64_t max_dt;
    static uint32_t t_win;
    if (dt > max_dt) max_dt = dt;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (now - t_win > 1000) {
        if (max_dt > 5000) ESP_LOGI(TAG, "handler max %lld us/s", (long long)max_dt);
        max_dt = 0;
        t_win = now;
    }
}
