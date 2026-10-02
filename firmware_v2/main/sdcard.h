#pragma once
#include <stdbool.h>
#include "esp_err.h"

#define SD_MOUNT "/sdcard"

// Mounts the card (1-bit SDMMC, FAT32/MBR). Never formats.
// Must run after lcd_init() — GPIO1/2 are shared with the LCD's init SPI.
esp_err_t sdcard_init(void);
bool sdcard_ok(void);
