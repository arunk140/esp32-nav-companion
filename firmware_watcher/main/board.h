#pragma once
// Board support: Seeed Studio SenseCAP Watcher W1-A.
// Pin map: docs-watcher/pinout.md (extracted from the vendor BSP, with
// file:line citations). Read that before changing anything here.
//
// Three things about this board shape the code below:
//   1. Power is switched by the IO expander, not by GPIO. Nothing — panel,
//      card, touch — works until board_exio_init() has raised the rails, and
//      dropping EXIO_PWR_SYSTEM powers the device off.
//   2. There are two I2C buses: I2C0 "general" (expander, RTC, codecs) and
//      I2C1 (touch only). The Waveshare board had one shared bus.
//   3. The expander is a 16-bit PCA9535, not the Waveshare's 8-bit TCA9554.
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

// --- I2C0: "general" bus (expander, PCF8563 RTC, ES8311/ES7243 codecs) ---
#define BOARD_I2C_NUM I2C_NUM_0
#define BOARD_I2C_SDA 47
#define BOARD_I2C_SCL 48
#define BOARD_I2C_HZ 400000

// --- I2C1: touch only (SPD2010) ---
#define BOARD_TOUCH_I2C_NUM I2C_NUM_1
#define BOARD_TOUCH_I2C_SDA 39
#define BOARD_TOUCH_I2C_SCL 38

#define BOARD_LCD_W 412
#define BOARD_LCD_H 412

// This firmware's screens were laid out by hand against the Waveshare board's
// 480x480 panel. UI_SC() maps those design-space pixel values onto the panel
// actually fitted, so the compositions survive the change of resolution: the
// SenseCAP Watcher is 412x412, a scale of 0.858.
//
// Views scale their absolute coordinates and sizes with it. Anything derived
// from BOARD_LCD_W/H, or positioned with LV_ALIGN_*, is already relative and
// needs nothing. Where a literal is genuinely a design-space value, the call
// site keeps the original number wrapped in UI_SC() so the layout still reads
// the way it was designed.
#define UI_DESIGN 480
#define UI_SC(n) (((n) * BOARD_LCD_W + UI_DESIGN / 2) / UI_DESIGN)
#define UI_SCF(n) ((n) * BOARD_LCD_W / (float)UI_DESIGN)

// --- PCA9535 expander: 16-bit, address 0x21, on I2C0 ---
// Bits are *linear*: bit n is port n/8, pin n%8. This matches the vendor BSP,
// where IO_EXPANDER_PIN_NUM_n is the IDF's (1 << n) bit mask.
//
// Direction, from the vendor's DRV_IO_EXP_INPUT_MASK 0x20FF / OUT 0xDF00:
//   inputs  = bits 0..7 and 13   (0x20FF)
//   outputs = bits 8..12, 14, 15 (0xDF00)
// Anything below marked (in) is read-only on the chip; board_exio_set() on one
// of those bits is a silent no-op at the hardware level, so don't.
#define EXIO_BAT_CHRG_DET 0    // (in) charger active
#define EXIO_BAT_STDBY_DET 1   // (in) charge complete
#define EXIO_VBUS_IN_DET 2     // (in) external 5 V present
#define EXIO_KNOB_BTN 3        // (in) wheel press, active low
#define EXIO_SD_DET 4          // (in) microSD present
#define EXIO_TOUCH_INT 5       // (in) SPD2010 touch interrupt, active low
#define EXIO_HIMAX_SYNC 6      // (in) Himax ready; unused by the nav firmware
#define EXIO_HIMAX_RST 7       // (in) — see docs-watcher/pinout.md, this bit is
                               //      in the *input* mask despite being named RST
#define EXIO_PWR_SD 8          // microSD rail
#define EXIO_PWR_LCD 9         // panel + backlight rail
#define EXIO_PWR_SYSTEM 10     // master rail — dropping this powers off
#define EXIO_PWR_AI 11         // Himax HX6538 rail; unused by the nav firmware
#define EXIO_PWR_CODEC_PA 12   // audio codec/PA rail; unused by the nav firmware
#define EXIO_BAT_DET 13        // (in) battery fitted
#define EXIO_PWR_GROVE 14      // Grove connector rail; unused by the nav firmware
#define EXIO_PWR_BAT_ADC 15    // battery divider enable; unused by the nav firmware

esp_err_t board_i2c_init(void);
i2c_master_bus_handle_t board_i2c_bus(void);

// I2C1. Must run before touch_init().
esp_err_t board_touch_i2c_init(void);
i2c_master_bus_handle_t board_touch_i2c_bus(void);

// Brings up the expander and raises the power rails. Must run after
// board_i2c_init() and before the LCD, touch or SD card.
esp_err_t board_exio_init(void);
esp_err_t board_exio_set(int bit, bool level);
bool board_exio_get(int bit);

// Backlight PWM on GPIO8 (LEDC channel 0). 0-100 %.
void board_backlight_init(void);
void board_backlight_set(uint8_t pct);

// The Watcher has no BOOT button: the long-press action maps to the wheel's
// press switch, which sits behind the expander. Polled by the GUI task.
void board_boot_button_init(void);
bool board_boot_button_pressed(void);

// Drops the peripheral rails ahead of deep sleep. Leaves EXIO_PWR_SYSTEM up so
// the device stays alive on its backup battery.
void board_power_rails_off(void);
