#pragma once
#include <stdint.h>
#include <stdbool.h>

// Screens and the gesture recogniser.
//
// The old tree hung LVGL event callbacks off widget objects and lost a
// weekend to events that do not bubble. Here the screen owns a button table
// and does its own hit testing from the touch sample: one recogniser, one
// place where a gesture can be dropped, and no dependence on which child
// happened to be topmost.

typedef enum { UI_PAIRING, UI_MAP } ui_screen_t;

void ui_init(void);
ui_screen_t ui_screen(void);
void ui_show_map(void);
void ui_show_pairing(void);

// Feed a frame: reads the touch sample, runs the recogniser, repaints what
// changed, and pushes dirty regions. GUI task, once per iteration.
void ui_frame(uint32_t now_ms);

// BLE link state, for the status line.
void ui_set_linked(bool linked);

