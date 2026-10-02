#pragma once
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

// Initializes the ST7701S (3-wire SPI init sequence, then RGB565 streaming)
// with three PSRAM frame buffers and internal bounce buffers.
// Releases the SPI bus afterwards: GPIO1/2 are then free for the SD card.
esp_err_t lcd_init(void);

esp_lcd_panel_handle_t lcd_panel(void);
void lcd_get_frame_buffers(void **fb0, void **fb1, void **fb2);

// Arms a one-shot notification for the next frame the panel starts scanning
// from the frame buffer set by the last draw_bitmap. Returns false on timeout.
void lcd_arm_frame_wait(void);
bool lcd_wait_frame(uint32_t timeout_ms);
