#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "ble_service.h"
#include "lvgl.h"

// Turn-by-turn banner for the map screen. GUI task only.
//
// The icons are drawn from primitives (strokes + arrow heads) rather than
// shipped as bitmaps: nothing crosses the BLE link, they stay crisp at any
// size, and there are no assets to license. One colour, thick strokes, so
// they read at 56 px on a round screen.

void nav_view_init(void);
void nav_view_destroy(void);

// Latest state; call on every loop from any screen. Hidden while inactive.
void nav_view_update(const nav_state_t *st, const char *street);

// True once a live instruction has arrived; the map screen uses it to swap
// the status pill for the banner.
bool nav_view_active(void);

// Remaining time to arrival in seconds, from the last instruction. Only
// meaningful while nav_view_active(); 0 otherwise.
uint16_t nav_view_remaining_s(void);

// Drop the current instruction (link lost / phone gone). Hides immediately
// instead of waiting for the staleness timer.
void nav_view_clear(void);

// The banner is tappable while shown. Wire this to the same action as the
// recentre button, which sits underneath the banner and is hidden during nav.
void nav_view_set_click_cb(void (*cb)(void));
