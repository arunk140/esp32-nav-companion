#pragma once
#include "lvgl.h"

// LVGL 9 on the RGB panel: FULL render mode straight into the panel's three
// frame buffers (no copies, no tearing, no vsync stalls in the GUI task).
// Touch indev reads touch_get() (never the chip).
void display_init(void);
lv_display_t *display_get(void);

uint32_t display_fps(void);           // frames presented in the last second
uint32_t display_last_draw_ms(void);  // render time of the last frame
uint32_t display_last_wait_ms(void);  // time blocked waiting for scan-out
