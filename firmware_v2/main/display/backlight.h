#pragma once
#include <stdint.h>

// Backlight on GPIO6, LEDC PWM >= 20 kHz (inaudible).
// Remote range 0-100 via CMD 0x05; local floor at 5 so the screen
// can never be blacked out remotely. Persisted in NVS.
#define NAV_BACKLIGHT_GPIO 6
#define NAV_BRIGHTNESS_MIN 5
#define NAV_BRIGHTNESS_MAX 100
#define NAV_BRIGHTNESS_DEFAULT 80

void display_set_brightness(uint8_t pct);
uint8_t display_get_brightness(void);
void backlight_init(void);  // call once after LCD_Init; restores NVS value
