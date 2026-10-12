#pragma once
#include "lvgl.h"

// Round icon button centred at (x, y), diameter d. White, flat, dims when
// disabled. Shared by the map and media pages so they look identical.
lv_obj_t *ui_round_button(lv_obj_t *parent, const char *sym, int x, int y, int d,
                          const lv_font_t *font, lv_event_cb_t cb);
