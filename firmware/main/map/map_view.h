#pragma once
#include "lvgl.h"

// Browse-only map (no location). 3x3 grid of 256px RGB565 tiles from
// /sdcard/street_map, drag-pan + pinch/button zoom, z15-17.
// Marker arrives with M3 (BLE position).

void map_view_create(lv_obj_t *parent);
void map_view_poll(void);    // pinch detection; call from gui task
void map_view_destroy(void); // frees tiles handle + screen content
bool map_view_is_live(void); // true between create and destroy

// Live position marker (M3, BLE). Creates the marker on first call.
// live=true: blue dot. live=false: gray dot (stored fix, no phone).
//
// Fixes are not drawn verbatim: they feed a time-constant ease (and a small
// noise deadband, plus a snap for teleports), so a 1 Hz stream renders as
// continuous motion. In follow mode the map centre chases the same eased
// value, so the map and the dot can never disagree.
void map_view_set_position(double lat, double lon, bool live);

// Programmatic zoom for CMD 0x02. Anchored on the current map centre.
void map_view_set_zoom(int z);

// Recenter on live fix if available, else the browse default (CMD 0x01).
void map_view_recenter_default(void);

