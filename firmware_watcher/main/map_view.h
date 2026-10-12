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
// Destination pin. Call with valid == false to clear it.
void map_view_set_dest(bool valid, int32_t lat_e7, int32_t lon_e7);

// Route line (from a Valhalla route). Assemble with reset -> add* -> commit;
// call clear to drop it. GUI task only.
void map_view_route_reset(void);
void map_view_route_add(const int32_t *lat_e7, const int32_t *lon_e7, int n);
void map_view_route_commit(void);
void map_view_route_clear(void);
bool map_view_route_active(void);
void map_view_set_link(bool connected, bool paired);
float map_view_rotation(void);  // deg, 0 = north-up

// Writes any deferred position/zoom to NVS now (before deep sleep).
void map_view_flush_state(void);
bool map_view_is_moving(void);
void map_view_recenter(void);

// Called (from map_view_tick, never inside an LVGL event) when the status
// pill has been held for 15 s: same as holding BOOT for 5 s.
void map_view_set_reset_cb(void (*cb)(void));

// Shows a short message over the map. No-op on other screens.
void map_view_toast(const char *txt);

// Repaints after a tile-theme change (placeholder colours follow it).
void map_view_invalidate(void);

// Called from the music button's click event: the callee must defer any
// screen change (it would delete the button inside its own event).
void map_view_set_media_cb(void (*cb)(void));

// Triple tap on the status pill -> straight to deep sleep. Runs from
// map_view_tick, not from the event.
void map_view_set_sleep_cb(void (*cb)(void));
void map_view_set_zoom(int z);

// Congestion along the current route: one NAV_TRAFFIC write as it arrived.
// Colours the route line and the trip progress arc. GUI task only.
void map_view_traffic(const uint8_t *data, size_t len);

// Nearby streets, drawn dimly under the route in the minimal display. Takes
// one NAV_ROADS write as it arrived (see ble_service.h); a set becomes visible
// when its last chunk is in. GUI task only.
void map_view_roads_chunk(const uint8_t *data, size_t len);

// Minimal display: a black screen with the route line, a heading arrow and a
// trip progress arc. Reads no map tiles, so it works without an SD card and
// leaves the card idle. Heading-up, with the arrow below the centre so more
// of the road ahead shows.
void map_view_set_minimal(bool on);

// The phone's battery (percent, -1 unknown), its local time and whether it
// shows times on a 24-hour clock, for the minimal display's faces. The clock
// keeps running from the last value.
void map_view_set_phone(int battery_pct, bool charging, bool clock_24h, uint32_t local_epoch_s);

// The device's own battery (percent, -1 = unknown), shown on the battery
// face under the phone's.
void map_view_set_device_batt(int battery_pct, bool charging);

// True while the battery/brightness face is the one on screen.
bool map_view_brightness_face(void);

// The battery face has brightness buttons. cb(delta) changes the brightness by
// that many percent and returns the level in use; cb(0) only asks.
void map_view_set_brightness_cb(int (*cb)(int delta));

// A long press on the map asks to switch between the map and the minimal
// display. Runs from map_view_tick, not from the event.
void map_view_set_mode_cb(void (*cb)(void));
