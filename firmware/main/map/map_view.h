#pragma once
#include "lvgl.h"

// Browse-only map (no location). 3x3 grid of 256px RGB565 tiles from
// /sdcard/street_map, drag-pan + pinch/button zoom, z15-17.
// Marker arrives with M3 (BLE position).

void map_view_create(lv_obj_t *parent);
void map_view_poll(void);    // pinch detection; call from gui task
void map_view_destroy(void); // frees tiles handle + screen content
bool map_view_is_live(void); // true between create and destroy

// Live position marker (M3, BLE). Recenter only when the fix leaves the inner
// tile; otherwise just moves the marker. Creates the marker on first call.
// live=true: blue dot. live=false: gray dot (stored fix, no phone).
void map_view_set_position(double lat, double lon, bool live);

// Programmatic zoom for CMD 0x02.
void map_view_set_zoom(int z);

// Recenter on live fix if available, else the browse default (CMD 0x01).
void map_view_recenter_default(void);
