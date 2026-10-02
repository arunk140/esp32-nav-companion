#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"

// Album art pipeline: BLE ART chunks -> reassembly -> JPEG decode -> round
// mask -> double-buffered RGB565 image. Decoding runs in its own task.

// Background colour outside the round art (must match the media page bg).
#define MEDIA_ART_BG 0x101418

void media_art_init(void);

// NimBLE host task: copies one ART write and returns (never blocks).
void media_art_feed(const uint8_t *data, size_t len);

// GUI task: true when a newly decoded image is ready; *img then stays valid
// until the next successful poll. *art_id identifies the track.
bool media_art_poll(const lv_image_dsc_t **img, uint16_t *art_id);

// GUI task: art id currently being received (0 = idle) and ms since the
// last chunk arrived.
uint16_t media_art_rx_id(void);
uint32_t media_art_rx_idle_ms(void);
