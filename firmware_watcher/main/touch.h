#pragma once
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint8_t n;  // 0 = released, 1..2 = active points
    int16_t x[2];
    int16_t y[2];
} touch_sample_t;

esp_err_t touch_init(void);

// Reads the controller. GUI task only — this is the single reader; every
// consumer (LVGL indev, pinch gesture) uses touch_get().
void touch_poll(void);
const touch_sample_t *touch_get(void);
