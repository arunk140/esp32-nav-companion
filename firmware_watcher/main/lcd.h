#pragma once
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

// SPD2010 412x412 panel on SPI3 in quad mode (QSPI).
//
// There is no frame buffer to hand out: unlike the Waveshare board's RGB
// panel, this one is a plain SPI device that the CPU pushes pixels to. LVGL
// keeps its own buffers and display.c streams them through draw_bitmap().
//
// The panel has no reset GPIO (BSP_LCD_GPIO_RST is unconnected and the panel
// shares its reset with touch). Its power rail, raised in board_exio_init(),
// is what resets it.
esp_err_t lcd_init(void);

esp_lcd_panel_handle_t lcd_panel(void);

// Needed by display.c to register the colour-transfer-done callback, which is
// what tells LVGL a flush has finished.
esp_lcd_panel_io_handle_t lcd_io(void);
