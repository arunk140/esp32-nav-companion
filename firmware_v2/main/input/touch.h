#pragma once
#include <stdint.h>
#include <stdbool.h>

// Touch input: the GT911 is read here and ONLY here.
//
// The old tree ran the controller through LVGL's indev, which made the sample
// a shared resource -- esp_lcd_touch_get_coordinates() invalidates the sample
// on every read, so a second reader starved the first. With no widget layer
// there is exactly one reader by construction, and the sample is just this
// struct, refreshed once per GUI iteration.

#define TOUCH_MAX_POINTS 2

typedef struct {
    int n;                                  // contacts this sample (0..2)
    int x[TOUCH_MAX_POINTS];
    int y[TOUCH_MAX_POINTS];
    bool down;                              // any contact
    bool just_down;                         // 0 -> >=1 since last poll
    bool just_up;                           // >=1 -> 0 since last poll
} touch_sample_t;

// Read the controller and update the sample. GUI task only, once per
// iteration. Points outside the round safe area are reported as no contact.
void touch_poll(void);

const touch_sample_t *touch_get(void);
