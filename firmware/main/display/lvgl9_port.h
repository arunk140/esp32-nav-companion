#pragma once
#include "lvgl.h"

// LVGL 9 port over the vendor esp_lcd RGB panel (ST7701S.c) and GT911 touch.
// Replaces the vendor LVGL_Driver (which is LVGL 8). Uses extern panel_handle
// (LCD_Driver/ST7701S.h) and extern tp (Touch_Driver/GT911.h) — both must be
// initialised (LCD_Init, Touch_Init) before calling lvgl9_init().

// 480x480, RGB565. Partial draw buffers in PSRAM (480x40 lines x2).
void lvgl9_init(void);

// Must be called in a loop (or dedicated task) every ~5-10 ms.
void lvgl9_tick_loop(void);

// Last touch sample for non-LVGL consumers (pinch). See .c — never read the
// touch controller directly; get_coordinates() invalidates the sample.
void touch_last_sample(uint16_t *x, uint16_t *y, uint8_t *cnt);
