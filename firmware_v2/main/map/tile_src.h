#pragma once
#include <stdint.h>
#include <stdbool.h>

// Async SD tile reader.
//
// A tile is 128 KiB and the card is on a 1-bit bus at 40 MHz, so a read costs
// 65-130 ms. Doing that in the GUI task would freeze the UI for a tenth of a
// second, so the read happens on a loader task and the GUI task only ever
// memcpys a finished tile into the mosaic.
//
// On-disk format (unchanged from firmware v1 -- the same SD card must work):
//   /sdcard/street_map/{z}/{x}/{y}.bin
//   12-byte LVGL header (cf=0x19 RGB565, w=256, h=256, size=131072),
//   then 256*256*2 bytes of RGB565.

#define TILE_PX 256
#define TILE_BYTES (TILE_PX * TILE_PX * 2)
#define TILE_HEADER_BYTES 12

typedef struct {
    int slot;        // 0..8, index into the 3x3 grid
    int z, x, y;     // tile the request is for
    uint32_t gen;    // batch id; stale results are dropped by the consumer
} tile_req_t;

typedef struct {
    int slot;
    int z, x, y;
    uint32_t gen;
    bool ok;
    const uint8_t *scratch;  // valid iff ok; must be released
} tile_done_t;

void tile_src_init(void);
// Returns false if the request was dropped (queue full). The caller must
// only count a slot as loading when this returns true, and retry on false.
bool tile_src_enqueue(const tile_req_t *req);
// One completion, or false when the queue is empty.
bool tile_src_poll(tile_done_t *out);
void tile_src_release(const uint8_t *scratch);
// Accepted-but-unreported count: an O(1) "is anything in flight" test.
int tile_src_active(void);
