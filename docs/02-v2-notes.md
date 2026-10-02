# firmware_v2 — notes

A clean rewrite of the nav firmware. `firmware/` (v1, LVGL-based) is kept for
reference and still builds; v2 is a separate project directory with its own
`sdkconfig`.

## What is identical to v1 (contracts — do not change)

- **BLE GATT.** Service `e8dffd56-e8b9-4279-9bff-1e0d2220d776`; characteristics
  POSITION `26a57cd7-…`, CMD `b18e4af0-…`, STATUS `d7865290-…`. The Android app
  in `app/` mirrors these and needs no edits.
- **POSITION payload.** 18 B little-endian: `i32 lat_e7, i32 lon_e7,
  u16 speed_cm_s, u16 cog_cdeg, u8 sats, u8 fix, u32 ts_ms`.
- **CMD codes.** 0x01 recenter, 0x02 set zoom, 0x03 ping, 0x04 unpair,
  0x05 brightness, 0x06 pair.
- **Pairing QR payload.** Byte-identical JSON, because the scanner parses it:
  `{"n":…,"m":…,"s":…,"t":"%06lu"}`.
- **NVS.** Same namespace `nav` and the same keys, so a device already paired
  to v1 boots straight into the v2 map.
- **SD tile format.** `/sdcard/street_map/{z}/{x}/{y}.bin`, 131,084 B:
  a 12-byte LVGL header (cf `0x19` RGB565, w 256, h 256, size 131072) then
  256×256×2 of RGB565. The same card works with both firmwares.

## What is different

No LVGL at all. The UI is a map, a dot, three buttons and a status line, and
every bug that cost time in v1 lived in the widget/event layer rather than in
the map maths.

| Concern | v1 | v2 |
| --- | --- | --- |
| Framebuffer | LVGL partial strips, 40 lines | own 480×480 RGB565 array in PSRAM, dirty **row bands** |
| Tile store | 9 rotating `lv_image` slots + generation guards | one 768×768 mosaic in PSRAM that *is* the cache |
| Touch | LVGL indev, callbacks on `s_box`, event bubbling | GT911 read once per frame by the GUI task; own gesture state machine |
| Hit testing | LVGL indev hit test (deepest clickable child) | own button table + `hit()` |
| Text | LVGL Montserrat fonts | 5×7 bitmap font, uppercase, ~180 bytes |
| Binary size | 1,042 KB | 708 KB |

### Why dirty tracking is row bands, not rectangles

The panel is an RGB LCD configured with `psram_trans_align = 64`, so a
transfer must start on a 64-byte boundary. A full-width row is 480×2 = 960
bytes and 960 is a multiple of 64, so `y*960` is always aligned; a dirty
rectangle starting at an arbitrary `x` is not. v1 never hit this because its
flushes were always full width. It still pays: a status-bar repaint pushes
480×30, and an idle screen pushes nothing.

### Why the zoom has no scaled preview

v1 scaled the tile container to fake continuity while the new zoom loaded, and
zoom-out produced a **white flash**: the container is 768 px on a 480 px screen,
so it only covers the display by being larger than it, and a 0.5× preview
shrank it below the screen. There was also no theme in v1, so the exposed
background was LVGL's opaque default white.

v2 does not scale anything. After a zoom, each mosaic cell already holds the
right *area* at the previous scale (grid cell (r,c) wanted tile
`mtx+c, mty+r` and still wants it), so the mosaic is deliberately **not**
shifted and the map visibly sharpens as tiles land.

## Things that will bite again

- **`pdMS_TO_TICKS(5)` is ZERO** at `CONFIG_FREERTOS_HZ=100`. A tick is 10 ms.
  `vTaskDelay(pdMS_TO_TICKS(5))` therefore returns immediately and the GUI
  task becomes a ~29 kHz free-running spin, which saturates CPU 1, starves
  IDLE1 and trips the 5 s task watchdog. Use `vTaskDelay(1)`. This cost real
  time in v1 and is not obvious from the source.
- **The USB console handle dies on a hard reset.** After `idf.py flash` the
  native USB port re-enumerates, so a `serial.Serial()` opened before the reset
  reads nothing at all. For a clean capture use
  `esptool.py --chip esp32s3 --port /dev/ttyACM0 run` (soft reset, no
  re-enumeration) and *then* open the port.
- **Something on the desktop keeps `/dev/ttyACM0` open** (VS Code holds it
  read-write, FD 80, and respawns the holder). Kill the holder before flashing:
  `P=$(lsof -t /dev/ttyACM0); kill $P`.
- **Ship at WARN.** `CONFIG_LOG_DEFAULT_LEVEL_WARN=y` in both `sdkconfig` and
  `sdkconfig.defaults`; the USB console blocks when no reader is attached and
  an INFO log in a hot path wedges the BLE host task. For a dev build, set the
  generated `sdkconfig` to INFO and rebuild — do not edit the defaults.
- **`CONFIG_IDF_TARGET="esp32s3"` in sdkconfig.defaults.** Without it a fresh
  checkout defaults to esp32, silently drops the S3 LCD capability flags, and
  then fails on `esp_lcd_panel_rgb.h`.
- **`sdkconfig` lies by omission.** When adding an option, set it in *both*
  `sdkconfig` and `sdkconfig.defaults`. Note that v2's `sdkconfig` was seeded
  from v1's so the board hardware capability flags (LCD RGB, octal PSRAM) are
  present; a from-scratch build must still get them from the defaults, and the
  S3 `SOC_LCDCAM_RGB_LCD_SUPPORTED` is `default y` so nothing extra is needed.

## Rendering gotchas found the hard way

Four bugs, all of which looked like "the map is glitched" from the glass. They
were found by dumping the framebuffer over serial and looking at it as an
image, which is worth doing again the next time the screen is wrong: reasoning
about it from source produced two confident wrong answers first.

1. **A cell in the mosaic is not contiguous.** The mosaic is 768 px wide with a
   768 px stride, so a 256×256 cell is 256 *separate* rows. Writing a cell with
   one `memcpy(TILE_BYTES)` spills each row into the neighbouring cell and
   walks off the end of the mosaic. Symptom: the map was mostly gap colour with
   one scrambled band, while every cell reported itself loaded. Fix: write row
   by row. (`mapview.c`, tile commit.)
2. **A strided blit must not pre-offset its source.** Clipping adjusts the
   source row and column; passing a pointer that is *already* offset and then
   clipping skips the source twice and shows a different part of the image.
   `gfx_blit_region()` takes the source origin explicitly and refuses to read
   past `src_h`.
3. **A "dirty" mark is not a repaint.** Marking a region dirty only re-pushes
   whatever is already in the framebuffer. When a tile lands, the viewport has
   to be re-rendered, or the screen keeps showing the previous content.
4. **A rounded rect that is a circle has zero-width centre rects.** The usual
   decomposition (centre rect + two side rects + corner arcs) degenerates when
   `r*2 == w`: the three fills become no-ops and only the arc specks are drawn.
   All three buttons are 56 px with r=28, so they rendered as hollow dust.
   `gfx_round_rect()` derives one span per row from the corner circle instead.

### Looking at the screen from the host

The panel is not readable from the host, so the framebuffer was dumped over
serial (downscaled 2× to 240×240 RGB565 = 115200 B, ~10 s) with `FBDUMP
<w> <h> BEGIN` / `FBDUMP END` markers, and reassembled into a PNG. That code has
been removed; re-add it if the rendering is ever wrong again. Two gotchas:

- Capture with `esptool.py --chip esp32s3 --port /dev/ttyACM0 run` (soft reset)
  and *then* open the port. A hard reset re-enumerates USB and leaves the
  handle reading nothing.
- The dump needs `CONFIG_LOG_DEFAULT_LEVEL_INFO=y`; it was removed with the
  rest of the diagnostics, so re-enable it in the generated `sdkconfig` for a
  dev build.

## Map model

All geographic state is in **absolute fractional tile coordinates** at the
current zoom. The grid origin is *derived* (`floor(centre) - 1`), never
tracked, so neither a pan nor a zoom can invalidate a stored coordinate.

Zoom anchored so a geo point keeps its screen position. Tile coordinates scale
by `k = 2^dz` and a tile is 256 px at every zoom (the geographic span per tile
changes, the pixel size does not), so preserving the anchor's offset from the
centre in tile units gives:

```
new_centre = centre + anchor_tile * (k - 1)
```

with the anchor at the centre degenerating to `anchor_tile * k`, i.e. an exact
hold. Verified to 0–1 px for centre and off-centre anchors in both directions.

The position filter is a time-constant ease (τ = 0.35 s) with a sub-pixel stop
threshold, a per-frame speed cap, and a snap for teleports (> 150 m, e.g. a
Doze batch or a reconnect). There is deliberately **no deadband**: a deadband
freezes the smoothed value while the vehicle keeps moving, so the target walks
away and the map leaps when the gate reopens. v1's 15 m gate did exactly that,
producing a 6.6 px leap every second at 50 km/h.

## Verified on hardware

- Clean boot; panel, touch, SD and BLE all initialise; 7552 K PSRAM pool.
- 9 tiles read from `/sdcard/street_map` in ~3 s at z17.
- **The rendered frame was inspected as an image** (framebuffer dumped and
  reassembled to PNG): a full, coherent 3×3 grid of Mumbai street tiles, no
  gaps, no scrambled band, status bar and buttons correct.
- Zoom buttons cycle 15↔16↔17 and clamp at the limits.
- 110 s soak on the shipping (WARN) build: no watchdog trip, no panic.

## Not verified on hardware

- Live GPS: no phone was paired, so the follow path and the dot easing under a
  1 Hz stream rest on the geometry model, not on a device.
- Pinch: needs two fingers.
- Motion while panning: a still frame cannot show whether the pan is smooth,
  only that the geometry is right.
