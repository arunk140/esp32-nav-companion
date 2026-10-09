#pragma once
#include <stdbool.h>
#include "lvgl.h"

// Start-up animation: a ring that draws itself round the rim with its tick
// marks, two arcs sweeping in opposite directions inside it, then the heading
// arrow and the device's name. About two seconds. `quick` plays it faster; it
// is not used at present (a wake from deep sleep plays the full version).
// GUI task only.
//
// on_done is called once from the animation's timer when it has played out;
// the callee must defer the screen change (it deletes this view).
void boot_view_create(lv_obj_t *parent, bool quick, const char *name, void (*on_done)(void));
void boot_view_destroy(void);
