#include "display/backlight.h"
#include "settings.h"
#include "LCD_Driver/ST7701S.h"  // Set_Backlight (LEDC owned by vendor LCD_Init)

// Single LEDC owner is the vendor Backlight_Init() (called from LCD_Init).
// This file only maps our 0-100 API + NVS restore onto it.

static uint8_t s_pct = NAV_BRIGHTNESS_DEFAULT;

void display_set_brightness(uint8_t pct)
{
    if (pct > 100) pct = 100;
    uint8_t applied = pct < NAV_BRIGHTNESS_MIN ? NAV_BRIGHTNESS_MIN : pct;
    Set_Backlight(applied);
    s_pct = pct;
}

uint8_t display_get_brightness(void)
{
    return s_pct;
}

// Called once at boot, after LCD_Init. Restores NVS value.
void backlight_init(void)
{
    display_set_brightness(settings_get_brightness());
}
