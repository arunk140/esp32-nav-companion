#pragma once
#include <stdint.h>
#include <stdbool.h>

// Map model.
//
// Everything geographic lives in ABSOLUTE fractional tile coordinates at the
// current zoom. That single choice is what makes panning and zooming both
// trivial: the grid origin is derived (floor(centre) - 1) rather than tracked,
// so a shift or a zoom never invalidates a stored coordinate, and there is no
// "adjust the marker when the grid moves" bookkeeping to get wrong. (The v1
// tree stored the marker in grid-relative units and needed a frame_shift()
// helper; forgetting to call it is what put the dot 1.2e7 px off the grid.)
//
// Rendering is one 3x3 mosaic of tiles in PSRAM, blitted to the framebuffer
// with a sub-tile offset. The mosaic doubles as the tile cache: panning slides
// it, and a slot that is not resident keeps its previous pixels rather than
// going blank.

#define MAP_ZOOM_MIN 15
#define MAP_ZOOM_MAX 17
#define MAP_DEFAULT_ZOOM 16
#define MAP_GRID 3
#define MAP_TILE_PX 256
#define MAP_MOSAIC_PX (MAP_GRID * MAP_TILE_PX)  // 768
#define MAP_DEFAULT_LAT 18.9219
#define MAP_DEFAULT_LON 72.8347

// Dot position as a fraction of the screen, for the UI to label.
typedef struct {
    bool have_fix;     // a position has ever been received
    bool have_stored;  // a persisted position exists
    double lat, lon;   // latest accepted fix
    bool have_speed;
    int speed_cms;
} map_status_t;

// The rectangle the map draws into, in screen pixels. The UI reserves a status
// bar, and the map must not paint over it: a full-width blit would erase the
// bar and the 1 Hz status repaint would have to fight it. Defaults to the whole
// screen. Call before mapview_init().
void mapview_set_viewport(int x0, int y0, int w, int h);

// Create the mosaic. Call once, after the SD card is mounted.
bool mapview_init(void);
void mapview_destroy(void);

void mapview_set_zoom(int z);
int  mapview_get_zoom(void);
void mapview_zoom_step(int delta);

// CMD 0x01 / the C button: live fix > stored fix > browse default, and
// re-engage follow mode.
void mapview_recenter(void);

// A position from the phone. live=false means a persisted fix with no phone.
void mapview_set_position(double lat, double lon, bool live);

// Pan by a screen-space delta (finger moved right => map moves right).
void mapview_pan(int dx, int dy);

// Speed from the POSITION payload, cm/s; negative clears it.
void mapview_set_speed(int cms);

// Pinch step. mid_x/mid_y is the screen point to keep fixed.
void mapview_pinch(int delta, int mid_x, int mid_y);

// Per-iteration update: ease the centre and the fix, keep the mosaic aligned
// with the grid, drain finished tile loads, and mark dirty regions. GUI task.
void mapview_update(uint32_t now_ms);


const map_status_t *mapview_status(void);
uint32_t mapview_tiles_loaded(void);
