#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// NVS-backed settings. Namespace "nav".
// brightness: 0-100 (5 floor enforced at apply time, not storage).
// token: 6-digit pairing token, generated once.
// device_name: "NAV-XXXX" from eFuse MAC, generated once.

esp_err_t settings_init(void);

uint8_t settings_get_brightness(void);
esp_err_t settings_set_brightness(uint8_t pct);

uint32_t settings_get_token(void);  // generates + stores on first call

// Returns pointer to static "NAV-XXXX" string. Generates once from MAC.
const char *settings_get_device_name(void);

// True if a phone has completed pairing (bond + token verify, M3).
bool settings_get_paired(void);
esp_err_t settings_set_paired(bool paired);

// Last known position, int32 1e-7 degrees. Written throttled (not every fix)
// so flash isn't worn by a 1 Hz stream. Survives reboot for the gray marker.
bool settings_get_last_pos(int32_t *lat_e7, int32_t *lon_e7);
esp_err_t settings_set_last_pos(int32_t lat_e7, int32_t lon_e7);

// Map zoom (15-17), restored on boot. Written on every change (rare).
uint8_t settings_get_zoom(void);
esp_err_t settings_set_zoom(uint8_t zoom);

// Erase bond state: paired=false. Caller also clears NimBLE bonds (M3).
esp_err_t settings_factory_unpair(void);
