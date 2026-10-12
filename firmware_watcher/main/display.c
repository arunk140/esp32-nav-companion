#include "display.h"
#include "board.h"
#include "lcd.h"
#include "touch.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "display";

// LVGL 9 in FULL render mode, into two PSRAM buffers of our own.
//
// Why full-size buffers rather than a strip: map_view.c paints the whole map
// straight into lv_display_get_buf_active() on LV_EVENT_RENDER_START, so LVGL
// must hand it a complete frame. That contract is unchanged from the Waveshare
// build — and it is why this file still owns the buffers even though the panel
// no longer supplies them.
//
// What did change: the Waveshare panel was an RGB panel that scanned out of
// those buffers, so flushing was just a pointer switch and the panel ISR told
// us when scan-out had moved on. This panel is a QSPI device we push pixels
// to, so flushing is a real DMA transfer and "done" comes from the panel IO's
// colour-transfer-done callback.
static lv_display_t *s_disp;
static uint8_t *s_buf[2];
static SemaphoreHandle_t s_frame_sem;
// Set by flush_cb, cleared by the ISR: the completion that matters is the one
// for the frame we just queued, not a stale one.
static volatile bool s_frame_armed;
static uint32_t s_frames, s_fps, s_fps_t0;
static int64_t s_render_t0;
static int64_t s_flush_t0;
static uint32_t s_draw_ms, s_xfer_ms;
static uint32_t s_xfer_timeouts;

// The panel is square but the visible area is round: ignore touches in the
// corners hidden behind the bezel. The Waveshare board masked at 236 of a
// 240 half-width (98%); keep the same fraction.
#define BEZEL_RADIUS ((BOARD_LCD_W / 2) * 98 / 100)

static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Called from the SPI ISR once the last chunk of a draw_bitmap has been sent.
// Deliberately *not* IRAM_ATTR: ESP-IDF masks non-IRAM interrupts while the
// flash cache is off (NVS writes), so this runs after the cache is back. The
// callback is in flash, so marking it IRAM would not make it safe on its own.
static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
                               esp_lcd_panel_io_event_data_t *edata,
                               void *user_ctx)
{
    (void)io;
    (void)edata;
    BaseType_t woken = pdFALSE;
    s_xfer_ms = (uint32_t)((esp_timer_get_time() - s_flush_t0) / 1000);
    lv_display_flush_ready((lv_display_t *)user_ctx);
    if (s_frame_armed) {
        s_frame_armed = false;
        xSemaphoreGiveFromISR(s_frame_sem, &woken);
    }
    return woken == pdTRUE;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    (void)disp;
    (void)area;
    s_draw_ms = (uint32_t)((esp_timer_get_time() - s_render_t0) / 1000);
    s_flush_t0 = esp_timer_get_time();
    // Full-frame transfer: the draw buffer is the whole screen, so the area is
    // always 0,0..W,H. The DMA runs in the background; completion arrives in
    // on_color_trans_done.
    esp_lcd_panel_draw_bitmap(lcd_panel(), 0, 0, BOARD_LCD_W, BOARD_LCD_H, px);

    // Arm a one-shot notification for this frame, matching the pattern
    // firmware_v2 used for the RGB panel's scan-out.
    xSemaphoreTake(s_frame_sem, 0);
    s_frame_armed = true;

    s_frames++;
    uint32_t now = tick_cb();
    if (now - s_fps_t0 >= 1000) {
        s_fps = s_frames;
        s_frames = 0;
        s_fps_t0 = now;
    }
}

// In double-buffered mode LVGL renders the next frame while the previous one is
// still being transferred, then calls this just before handing us the new
// buffer. It must not busy-wait here: LVGL's own wait_for_flushing() falls back
// to `while(disp->flushing);` when no wait callback is set, which would spin the
// GUI task for the whole ~17 ms transfer. So block on a semaphore the ISR
// releases, with a timeout so a stalled bus cannot wedge the GUI task.
static void flush_wait_cb(lv_display_t *disp)
{
    (void)disp;
    if (xSemaphoreTake(s_frame_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
        s_frame_armed = false;
        if (++s_xfer_timeouts <= 3) {
            ESP_LOGW(TAG, "panel transfer did not complete in 100 ms (timeout %lu)",
                     (unsigned long)s_xfer_timeouts);
        }
    }
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
    if (dx * dx + dy * dy > BEZEL_RADIUS * BEZEL_RADIUS) {
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

    const uint32_t stride = BOARD_LCD_W * 2;
    const uint32_t size = stride * BOARD_LCD_H;
    s_frame_sem = xSemaphoreCreateBinary();
    for (int i = 0; i < 2; i++) {
        s_buf[i] = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
        if (!s_buf[i]) {
            ESP_LOGE(TAG, "no PSRAM for draw buffer %d (%lu bytes)", i, (unsigned long)size);
            abort();
        }
        memset(s_buf[i], 0, size);
    }

    s_disp = lv_display_create(BOARD_LCD_W, BOARD_LCD_H);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, s_buf[0], s_buf[1], size, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_flush_wait_cb(s_disp, flush_wait_cb);
    lv_display_add_event_cb(s_disp, render_start_cb, LV_EVENT_RENDER_START, NULL);

    // Tells LVGL the flush is over. Registered here, not in lcd.c, so the
    // panel driver stays ignorant of the graphics stack.
    const esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = on_color_trans_done };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(lcd_io(), &cbs, s_disp));

    // The bottom layer is opaque by default for RGB565: it would paint all
    // 412x412 px under every frame (~11 ms of PSRAM writes) for nothing. Every
    // screen paints its own background.
    lv_obj_set_style_bg_opa(lv_display_get_layer_bottom(s_disp), LV_OPA_TRANSP, 0);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, indev_read_cb);
    lv_indev_set_display(indev, s_disp);

    ESP_LOGW(TAG, "display up: %dx%d, 2 full buffers in PSRAM", BOARD_LCD_W, BOARD_LCD_H);
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
    // On the Waveshare board this was time blocked in flush_wait_cb waiting for
    // the panel to scan the new buffer. There is no scan-out to wait for now;
    // the closest equivalent — and the number that matters — is how long the
    // QSPI transfer of the last frame took.
    return s_xfer_ms;
}
