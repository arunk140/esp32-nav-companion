#include "display.h"
#include "board.h"
#include "lcd.h"
#include "touch.h"
#include "esp_timer.h"

static lv_display_t *s_disp;
static lv_draw_buf_t s_buf3;
static uint32_t s_frames, s_fps, s_fps_t0;
static int64_t s_render_t0;
static uint32_t s_draw_ms, s_wait_ms;

static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    (void)disp;
    (void)area;
    s_draw_ms = (uint32_t)((esp_timer_get_time() - s_render_t0) / 1000);
    // px is one of the panel's own frame buffers: this only switches the
    // scan-out to it (no copy). Completion is signalled from the panel ISR
    // once scanning of the new buffer has begun; LVGL waits for that in
    // flush_wait_cb right before its next flush, by which time it has
    // already rendered the next frame into the third buffer.
    esp_lcd_panel_draw_bitmap(lcd_panel(), 0, 0, BOARD_LCD_W, BOARD_LCD_H, px);
    lcd_arm_frame_wait();

    s_frames++;
    uint32_t now = tick_cb();
    if (now - s_fps_t0 >= 1000) {
        s_fps = s_frames;
        s_frames = 0;
        s_fps_t0 = now;
    }
}

static void flush_wait_cb(lv_display_t *disp)
{
    (void)disp;
    int64_t t0 = esp_timer_get_time();
    lcd_wait_frame(50);
    s_wait_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
}

static void render_start_cb(lv_event_t *e)
{
    (void)e;
    s_render_t0 = esp_timer_get_time();
}

static void indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    const touch_sample_t *t = touch_get();
    if (t->n == 0) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    // Ignore the corners hidden behind the round bezel.
    int dx = t->x[0] - BOARD_LCD_W / 2;
    int dy = t->y[0] - BOARD_LCD_H / 2;
    if (dx * dx + dy * dy > 236 * 236) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    data->point.x = t->x[0];
    data->point.y = t->y[0];
    data->state = LV_INDEV_STATE_PRESSED;
}

void display_init(void)
{
    lv_init();
    lv_tick_set_cb(tick_cb);

    void *fb0 = NULL, *fb1 = NULL, *fb2 = NULL;
    lcd_get_frame_buffers(&fb0, &fb1, &fb2);
    const uint32_t stride = BOARD_LCD_W * 2;
    const uint32_t size = stride * BOARD_LCD_H;

    s_disp = lv_display_create(BOARD_LCD_W, BOARD_LCD_H);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, fb0, fb1, size, LV_DISPLAY_RENDER_MODE_FULL);
    lv_draw_buf_init(&s_buf3, BOARD_LCD_W, BOARD_LCD_H, LV_COLOR_FORMAT_RGB565, stride, fb2, size);
    lv_display_set_3rd_draw_buffer(s_disp, &s_buf3);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_flush_wait_cb(s_disp, flush_wait_cb);
    lv_display_add_event_cb(s_disp, render_start_cb, LV_EVENT_RENDER_START, NULL);
    // The bottom layer is opaque by default for RGB565: it would paint all
    // 480x480 px under every frame (~15 ms of PSRAM writes) for nothing.
    lv_obj_set_style_bg_opa(lv_display_get_layer_bottom(s_disp), LV_OPA_TRANSP, 0);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, indev_read_cb);
    lv_indev_set_display(indev, s_disp);
}

lv_display_t *display_get(void)
{
    return s_disp;
}

uint32_t display_fps(void)
{
    if (tick_cb() - s_fps_t0 > 2000) return 0;  // idle
    return s_fps;
}

uint32_t display_last_draw_ms(void)
{
    return s_draw_ms;
}

uint32_t display_last_wait_ms(void)
{
    return s_wait_ms;
}
