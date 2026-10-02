#pragma once
// Board support: Waveshare ESP32-S3-Touch-LCD-2.8C.
// Pin map: docs/waveshare-2.8c/02-pinout.md
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#define BOARD_I2C_SDA 15
#define BOARD_I2C_SCL 7
#define BOARD_I2C_HZ 400000

#define BOARD_LCD_W 480
#define BOARD_LCD_H 480

// TCA9554 expander bits (EXIO1..8 -> bit 0..7)
#define EXIO_LCD_RST 0
#define EXIO_TOUCH_RST 1
#define EXIO_LCD_CS 2
#define EXIO_SD_D3 3
#define EXIO_IMU_INT2 4
#define EXIO_IMU_INT1 5
#define EXIO_BUZZER 7

esp_err_t board_i2c_init(void);
i2c_master_bus_handle_t board_i2c_bus(void);

// Must run after board_i2c_init and before LCD / touch / SD.
esp_err_t board_exio_init(void);
esp_err_t board_exio_set(int bit, bool level);

// Backlight PWM on GPIO6. 0-100 %.
void board_backlight_init(void);
void board_backlight_set(uint8_t pct);

// BOOT button (GPIO0, active low).
void board_boot_button_init(void);
bool board_boot_button_pressed(void);
