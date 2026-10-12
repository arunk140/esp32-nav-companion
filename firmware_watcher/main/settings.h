#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// NVS-backed settings. Same namespace + keys as firmware v1, so an existing
// pairing (name, token, paired flag, last fix) survives the upgrade.
// All values are cached in RAM at init; getters never touch flash.
// Setters write NVS: call them from the GUI task only.

#define NAV_BRIGHTNESS_MIN 5
#define NAV_BRIGHTNESS_DEFAULT 80

esp_err_t settings_init(void);

const char *settings_device_name(void);
const char *settings_token_str(void);  // 6 ASCII digits
void settings_new_token(void);         // invalidates every old QR scan

bool settings_paired(void);
void settings_set_paired(bool paired);

uint8_t settings_brightness(void);
void settings_set_brightness(uint8_t pct);

bool settings_dark_map(void);
void settings_set_dark_map(bool dark);

// Minimal display: black screen, route line and next turn only, no map tiles.
bool settings_minimal(void);
void settings_set_minimal(bool on);

uint8_t settings_zoom(void);  // 0 = unset
void settings_set_zoom(uint8_t z);

bool settings_last_pos(int32_t *lat_e7, int32_t *lon_e7);
void settings_set_last_pos(int32_t lat_e7, int32_t lon_e7);
