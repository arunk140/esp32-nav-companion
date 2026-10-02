#pragma once
#include <stdint.h>

// Panel output. The vendor ST7701S driver owns the panel handle and the
// init sequence; this is only the "push a rectangle of RGB565 to the glass"
// step that the old LVGL port used to own via a flush callback.

// Lines per transfer. Bounds the size of any single panel transaction; a full
// 480x480 frame is 460 KB, which is far too much to push in one go.
#define PANEL_STRIP_LINES 40

// Send a w*h RGB565 block from src (which must be DMA-capable) to (x,y).
void panel_present(const uint16_t *src, int x, int y, int w, int h);
