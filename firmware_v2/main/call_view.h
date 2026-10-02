#pragma once
#include "lvgl.h"

// Full-screen incoming-call page. GUI task only. Caller name/number persist
// across create/destroy so the page can be rebuilt after a screen switch.
//
// on_answer / on_reject / on_dismiss run inside LVGL click events: the callee
// must defer any screen change (it would delete the button mid-event).

// Stores the caller for the next/current build. Updates the live labels when
// the page is already on screen (call waiting).
void call_view_set_caller(const char *name, const char *number);

void call_view_create(lv_obj_t *parent, void (*on_answer)(void),
                      void (*on_reject)(void), void (*on_dismiss)(void));
void call_view_destroy(void);

// Forgets the last caller (link dropped). No-op on the objects.
void call_view_clear(void);
