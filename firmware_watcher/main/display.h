#pragma once
#include "lvgl.h"

// LVGL 9 in FULL render mode into two PSRAM buffers, streamed to the SPD2010
// over QSPI. Full-frame buffers are required, not chosen: map_view paints into
// lv_display_get_buf_active() on RENDER_START.
// Touch indev reads touch_get() (never the chip).
void display_init(void);
lv_display_t *display_get(void);

uint32_t display_fps(void);           // frames presented in the last second
uint32_t display_last_draw_ms(void);  // render time of the last frame
uint32_t display_last_wait_ms(void);  // QSPI transfer time of the last frame
