#pragma once
#include <stdbool.h>
#include "ble_service.h"
#include "lvgl.h"

// Phone media controls page. GUI task only. Playback state and album art
// persist across create/destroy (art keeps arriving while the map is up).

// on_back is called from the back button's click event: the callee must
// defer the screen change.
void media_view_create(lv_obj_t *parent, void (*on_back)(void));
void media_view_destroy(void);
bool media_view_active(void);

// Every GUI loop iteration, on any screen.
void media_view_tick(void);

void media_view_set_state(const nav_media_state_t *st);

// Track title / artist for the current media state. Non-Latin characters are
// dropped (the board font has no glyphs for them).
void media_view_set_text(const char *title, const char *artist);
const char *media_view_title(void);
void media_view_set_link(bool connected);

// Called once when the phone's track changes while this page is on screen
// (the map uses it for a toast). Fires from the GUI task.
void media_view_set_change_cb(void (*cb)(void));
