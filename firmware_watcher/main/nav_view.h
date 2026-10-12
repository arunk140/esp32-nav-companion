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

// Call every GUI loop so the banner's staleness is re-evaluated even when no
// further state arrives (a stop then silence must still hide it).
void nav_view_tick(void);

// True once a live instruction has arrived; the map screen uses it to swap
// the status pill for the banner.
bool nav_view_active(void);

// Remaining time to arrival in seconds, from the last instruction. Only
// meaningful while nav_view_active(); 0 otherwise.
uint16_t nav_view_remaining_s(void);

// Drop the current instruction (link lost / phone gone). Hides immediately
// instead of waiting for the staleness timer.
void nav_view_clear(void);

// Minimal display: no box, a large distance over the street name, and the
// "left" figure replaced by the progress arc the map screen draws.
void nav_view_set_minimal(bool on);

// How much of the trip is done, 0..1000, measured against the longest
// remaining distance seen since navigation started (or since a reroute made
// it longer). -1 while not navigating. Remaining distance is a u16 on the
// wire, so a trip over 65.5 km reads 0 until it comes under that.
int nav_view_progress_permille(void);

// Posted speed limit (0 = unknown) in the units the phone shows speeds in.
// Dropped with the instruction when the link is lost; the units are kept.
void nav_view_set_limit(bool mph, uint16_t limit);
uint16_t nav_view_limit(void);
bool nav_view_mph(void);

// Metres to the next maneuver / to the destination, or -1 while not navigating.
int nav_view_dist_m(void);
int nav_view_remaining_m(void);

// Screen y of the banner's top edge. In the minimal display the map screen
// dims everything from just above here to the bottom of the screen.
int nav_view_banner_top(void);

// Keeps the banner off the screen whatever the state (the speedometer has no
// room for it). Navigation itself carries on.
void nav_view_set_suppressed(bool on);

// The banner is tappable while shown. Wire this to the same action as the
// recentre button, which sits underneath the banner and is hidden during nav.
void nav_view_set_click_cb(void (*cb)(void));
