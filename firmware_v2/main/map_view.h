#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

// Offline slippy map + GPS marker. GUI task only.
// Position/zoom/link state persists across create/destroy, so fixes that
// arrive while the pairing screen is up are not lost.

void map_view_init_state(void);  // once at boot (restores zoom + last fix)
void map_view_create(lv_obj_t *parent);
void map_view_destroy(void);
bool map_view_active(void);

// Call every GUI loop iteration: drains tile loads, animates the marker,
// handles staleness and deferred NVS saves.
void map_view_tick(void);

void map_view_set_fix(int32_t lat_e7, int32_t lon_e7, uint16_t speed_cms, uint16_t cog_cdeg);
void map_view_set_link(bool connected, bool paired);
float map_view_rotation(void);  // deg, 0 = north-up
void map_view_recenter(void);

// Called (from map_view_tick, never inside an LVGL event) when the status
// pill has been held for 15 s: same as holding BOOT for 5 s.
void map_view_set_reset_cb(void (*cb)(void));

// Shows a short message over the map. No-op on other screens.
void map_view_toast(const char *txt);

// Called from the music button's click event: the callee must defer any
// screen change (it would delete the button inside its own event).
void map_view_set_media_cb(void (*cb)(void));
void map_view_set_zoom(int z);
