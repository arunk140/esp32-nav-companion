#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

// Tile cache + async SD loader.
//
// SD layout: /sdcard/street_map/{z}/{x}/{y}.bin — 12-byte LVGL image header
// followed by 256x256 RGB565 (XYZ / slippy-map scheme).
//
// Ownership: every function here is GUI-task only. The loader task touches
// nothing but the pixel buffer of a slot in LOADING state; the GUI never
// draws a slot until the loader hands it back through the done queue.

#define TILE_PX 256

typedef enum {
    TILE_READY,    // drawable
    TILE_PENDING,  // queued or loading
    TILE_MISSING,  // not on the card (cached negative result)
} tile_state_t;

void tiles_init(void);

// Zoom levels found on the card; both 0 when the card has no tiles.
int tiles_zoom_min(void);
int tiles_zoom_max(void);

// Call once at the top of each map render: tiles looked up after this are
// pinned (not evictable) until the next render.
void tiles_begin_frame(void);

// Looks up a tile, queuing a load if absent. urgent = visible now.
// Returns the image when READY, else NULL with *state set.
const lv_image_dsc_t *tiles_get(int z, int x, int y, bool urgent, tile_state_t *state);

// Requests a tile if not cached and there is spare capacity (no pinning).
void tiles_prefetch(int z, int x, int y);

// Loads for other zoom levels are dropped by the loader.
void tiles_set_active_zoom(int z);

// Drains finished loads. Returns true if anything became READY or MISSING.
bool tiles_poll(void);

typedef struct {
    uint32_t loads;
    uint32_t avg_load_ms;
    uint32_t max_load_ms;
    uint32_t missing;
    uint32_t pending;
} tiles_stats_t;
void tiles_get_stats(tiles_stats_t *out);
