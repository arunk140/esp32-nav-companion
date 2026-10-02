#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Async tile loader: SD file I/O off the GUI task.
//
// Flow: GUI enqueues (slot, z, x, y, gen) -> loader reads the file into a
// scratch buffer -> GUI drains completions, memcpys scratch into the slot
// buffer (frame-atomic: only the GUI task touches LVGL + slot buffers),
// re-links the image, releases the scratch. Stale generations are dropped.
//
// 4 scratch buffers x 131072 B PSRAM. Queue depth 16.

#define LOADER_TILE_BYTES (256 * 256 * 2)

typedef struct {
    int slot, z, x, y;
    uint32_t gen;
} load_req_t;

typedef struct {
    int slot, z, x, y;
    uint32_t gen;
    bool ok;
    uint8_t *scratch;  // valid iff ok; release after memcpy
} load_done_t;

struct map_tiles_t;  // opaque; loader only needs the handle + load_file
typedef struct map_tiles_t *loader_map_handle_t;

// handle: map_tiles fork handle (for map_tiles_load_file). Call once.
void loader_init(loader_map_handle_t handle);

// Enqueue a read.
//
// Returns false if the request was DROPPED because the queue is full. This
// is load-bearing: the caller must only count a slot as "loading" when this
// returns true, and must retry on false. A void return here let the caller
// count requests that never existed, so a dropped tile could never complete
// and the zoom preview would stay stuck at 200% forever.
bool loader_enqueue(int slot, int z, int x, int y, uint32_t gen);

// Drain one completion. Returns false when the queue is empty.
bool loader_poll_done(load_done_t *out);

// Number of accepted requests not yet reported. O(1) "is anything loading",
// so a 100 Hz GUI poll does not need a queue peek every tick.
int loader_active(void);

// Return a scratch buffer after memcpy.
void loader_release(uint8_t *scratch);
