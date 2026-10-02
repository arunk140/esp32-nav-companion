#include "display/panel.h"
#include "LCD_Driver/ST7701S.h"  // panel_handle (drivers/ include path)
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "panel";
static uint32_t s_err_count;

// Push one rectangle. Blocking: esp_lcd_panel_draw_bitmap waits for the DMA to
// drain unless the panel was configured with .flags.async = true, which this
// board's driver does not set. So this is called with the GUI task's whole
// budget, and gfx_present() splits work into strips to keep any single stall
// short.
void panel_present(const uint16_t *src, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    esp_err_t err =
        esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + w, y + h, src);
    if (err != ESP_OK) {
        // Rate-limited: a failing push must not become a log flood, but a
        // silently ignored error here is indistinguishable from a broken
        // panel, so count them and report periodically.
        if ((s_err_count % 16) == 0)
            ESP_LOGE(TAG, "draw_bitmap(%d,%d,%d,%d) err %d (n=%u)", x, y, w, h,
                     (int)err, (unsigned)s_err_count);
        s_err_count++;
    }
}
