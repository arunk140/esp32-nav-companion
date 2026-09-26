# 10 — `0015/map_tiles` Reference and Architecture Assessment

Assessment of the components found for on-device map rendering, and how they fit
a nav device built on the ESP32-S3-Touch-LCD-2.8C.

**Purpose:** the proposed architecture is *maps on the SD card, position streamed
over BLE from a phone, heading from the on-board IMU*. These references implement
most of it. This document records what they actually do, what they don't, and the
three things that will bite.

Retrieved 2026-09-25. Source: GitHub and the Espressif component registry.

## The component set

| Repo | What it is | License |
| --- | --- | --- |
| [`0015/map_tiles`](https://github.com/0015/map_tiles) | ESP-IDF component. Loads 256×256 RGB565 tiles from a filesystem, converts GPS↔tile coords, exposes tiles as LVGL image objects. | MIT |
| [`0015/map_tiles_projects`](https://github.com/0015/map_tiles_projects) | Worked examples. `01.Simple_Map` and `02.ESP32-S3_Map_LoRa_GPS`. | MIT |
| [`0015/OfflineMapDownloader`](https://github.com/0015/OfflineMapDownloader) | Python/Flask app. Rectangle-select an area, download OSM or Esri tiles as `.zip` or `.mbtiles`. | MIT |
| `map_tiles/script/lvgl_map_tile_converter.py` | Multithreaded PNG→RGB565 converter with a 12-byte header. | MIT |

All by the same author (Eric Nam / ThatProject). `map_tiles` is at
[v1.3.0](https://components.espressif.com/components/0015/map_tiles/versions/1.3.0/readme)
on the Espressif registry — 460 total downloads, 322 of that version, `esp32s3`
listed as a supported target.

**Maturity caveat:** 78 stars, 11 forks, 7 commits, one maintainer. Read the source
rather than trusting the README. Two concrete problems are documented below.

## Architecture validation

`02.ESP32-S3_Map_LoRa_GPS` is **architecturally the same shape as this project**:
map tiles on SD, local rendering, and a *position that arrives over a slow radio*
rather than from a local GNSS. It streams coordinates as CSV at 0.1 Hz over LoRa
(`Mom,1,47.6175803,-122.1235667`).

Replace LoRa with BLE and the phone's GPS with a phone streaming position, and the
data flow is identical. That example is the best available reference for this
design and is worth reading in full.

It also confirms a hardware point: its `user_config.h` uses
`GPS_TX_PIN 43` / `GPS_RX_PIN 44` — the same two free GPIOs identified in
[02-pinout.md](02-pinout.md).

## What it does *not* do

The component provides **no navigation**. The complete API is:

- tile load / buffer / image access
- grid sizing, zoom, tile-type switching
- `gps_to_tile_xy` / `tile_xy_to_gps` / `set_center_from_gps`
- `is_gps_within_tiles`
- `get/set_marker_offset`

There is **no route, no polyline, no maneuver list, no ETA, no off-route
detection, no distance-to-turn.** The LoRa example adds Haversine distance and a
proximity alarm, which makes it a *tracker*, not navigation.

Turn-by-turn must be built. See
[09-hardware-notes-for-nav.md](09-hardware-notes-for-nav.md) and the plan below.

## Blocker 1 — LVGL version conflict

```yaml
# 0015/map_tiles/idf_component.yml
dependencies:
  lvgl/lvgl: ">=9.3.0"
  idf: ">=5.0"
```

The Waveshare 2.8C demo is **LVGL 8.3.10** (Arduino) / **`~8.2.0`** (ESP-IDF).
These will not link together.

### Resolution: do not port the vendor's LVGL glue

Only one vendor file is LVGL-dependent. Inspect the demo tree in
`examples/esp-idf/main/`:

| Vendor file | LVGL-dependent? |
| --- | --- |
| `LCD_Driver/ST7701S.c` | **No** — `esp_lcd_new_rgb_panel()`, plain esp_lcd |
| `Touch_Driver/GT911.c` + `esp_lcd_touch` | **No** — esp_lcd touch API |
| `SD_Card/SD_MMC.c` | No |
| `PCF85063`, `QMI8658`, `TCA9554PWR`, `BAT_Driver`, `Buzzer`, `Wireless` | No |
| **`LVGL_Driver/LVGL_Driver.c`** | **Yes** — the only file to replace |

So: keep the vendor hardware drivers, discard `LVGL_Driver.c`, and write a thin
LVGL 9 port over `esp_lcd`. The flush callback is essentially:

```c
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1,
                              area->x1 + area->x2, area->y1 + area->y2, px_map);
    lv_display_flush_ready(disp);
}
```

Roughly 150 lines total including the touch indev. This is far less work than an
LVGL 8→9 migration of the vendor code, and the vendor glue is demo quality
regardless.

## Blocker 2 — memory, and it caps the grid

From `map_tiles.cpp`, `map_tiles_load_tile()`:

```c
handle->tile_bufs[index] = heap_caps_malloc(
    MAP_TILES_TILE_SIZE * MAP_TILES_TILE_SIZE * MAP_TILES_BYTES_PER_PIXEL,
    handle->use_spiram ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : MALLOC_CAP_DMA);
```

`256 × 256 × 2` = **131,072 bytes per tile**, allocated once and **not freed until
`map_tiles_cleanup()`**. All tiles stay resident. There is no eviction and no LRU.

| Grid | PSRAM for tiles | Verdict |
| --- | ---: | --- |
| 3×3 | 1.13 MB | **Recommended.** 768×768 covers 480×480 with margin |
| 5×5 | 3.13 MB | Workable, tighter |
| 7×7 | 6.13 MB | Uncomfortable |
| 9×9 (`MAP_TILES_MAX_GRID_COLS`) | **10.13 MB** | **Exceeds 8 MB PSRAM — allocation fails** |

The README claims a max of 10; the header says 9. **Trust the header.**

Budget at 3×3: 1.13 MB tiles + 0.45–0.90 MB framebuffer(s) + LVGL heap + app,
inside 8 MB. Comfortable.

### Two inefficiencies worth knowing

- `memset(tile_bufs[index], 0, 131072)` runs on **every** tile load — a full
  128 KB clear before the read.
- No cache: every `map_tiles_load_tile()` re-reads 128 KB from the SD card. On
  this board the card is **1-bit**, so budget ~65–130 ms per tile reload. Do not
  design anything that reloads tiles in a tight loop.

## Bug — the centring jitter

`map_tiles_set_center_from_gps()`:

```c
map_tiles_gps_to_tile_xy(handle, lat, lon, &x, &y);
handle->tile_x = (int)x - handle->grid_cols / 2;
handle->tile_y = (int)y - handle->grid_rows / 2;
handle->marker_offset_x = (int)((x - (int)x) * MAP_TILES_TILE_SIZE);
handle->marker_offset_y = (int)((y - (int)y) * MAP_TILES_TILE_SIZE);
```

With `grid_cols = 3`, integer division gives `3/2 = 1`, so `tile_x = floor(x) - 1`
and the grid spans `floor(x)-1 … floor(x)+1`. But `map_tiles_get_center_gps()`
computes the centre as `tile_x + grid_cols/2.0` = `floor(x) + 0.5` — the middle of
the centre tile, **not** the GPS position.

**Result: the map origin jumps by up to 128 px as the GPS moves within a tile.**
Combined with the absent tile cache, that also means near-continuous SD reloading.

The v1.3.0 changelog is the tell:

> "Fixed critical marker positioning bugs **in example code**: Bug Fix #1:
> Corrected `map_display_add_marker()` to calculate position from actual GPS
> coordinates instead of using stored offset"

The workaround was applied in the *example*, not the component. **Plan on forking.**

### Suggested fixes

1. Position the grid so the GPS point lands at the grid centre, and compute the
   marker offset as a fraction of the **grid**, not of a single tile.
2. Do not recentre on every position update. Recentre only when the GPS exits the
   inner region, and translate the map locally in between.
3. Consider a small tile cache in front of the component, so recentring does not
   re-read from SD.

The GPS↔tile maths itself (`gps_to_tile_xy` / `tile_xy_to_gps`) is standard
Web Mercator and is correct — only the grid origin is wrong.

## Tile format

```
{base_path}/{tile_folder}/{zoom}/{tile_x}/{tile_y}.bin
```

- 12-byte header (skipped with `fseek`, then the component builds its own
  `lv_image_dsc_t`, so **the header content is cosmetic**)
- Then 131,072 bytes of raw RGB565
- **256×256, no scaling, no sub-tiles**

Produced by `lvgl_map_tile_converter.py` (Pillow, multithreaded, `--jobs`,
`--force`). Zoom levels 10–19 per the LoRa example.

## Storage sizing

128 KB per tile is heavy. A 32 GB card holds **~244,000 tiles**.

For a 30 × 30 km region at mid-latitude:

| Zoom | Metres/tile | Tiles across | Total (15–17) |
| --- | ---: | ---: | ---: |
| 15 | ~320 m | 94 | ~0.9 M |
| 16 | ~160 m | 188 | ~3.5 M |
| 17 | ~80 m | 375 | ~14 M |

**Practical recommendation: zoom 15–17 over a city region, roughly 6–8 GB.** Skip
zoom 10–14 (no useful detail on a 2.8" screen) and 19 (large, low value). Format
the card **FAT32**, per the Waveshare FAQ.

## Pipeline

```
OfflineMapDownloader (Flask, rectangle select, OSM/Esri, z10-19)
        │  tiles.zip  (PNG, zoom/x/y.png)
        ▼
lvgl_map_tile_converter.py  (--input --output --jobs)
        │  zoom/x/y.bin  (12-byte header + RGB565)
        ▼
TF card  →  /street_map/{z}/{x}/{y}.bin
```

`OfflineMapDownloader` also emits `tiles.mbtiles` (SQLite), which the converter
does not read — use the `.zip` output for this pipeline.

### Licensing caveat

Quoted from the author's own README:

> "This project is intended for **personal, educational, or experimental use only**.
> […] **Do not use this tool for commercial applications or large-scale automated
> downloads.** Please respect the tile providers' usage policies."

It fetches from unauthenticated public endpoints (OpenStreetMap, ArcGIS). Fine
for personal testing; a city-wide download is precisely the use the author warns
against. Move to a proper tile provider before going further. Attribution is
required: © OpenStreetMap contributors, Esri.

## The IMU is not a compass

The QMI8658 is **6-axis — 3-axis accelerometer + 3-axis gyroscope. No
magnetometer.**

Consequences:

- It **cannot** produce absolute heading. Gyro-integrated yaw drifts, and there
  is no magnetic reference to correct it.
- It **cannot** replace a compass for heading-up navigation.
- It **cannot** substitute for a GNSS module.

What it is good for:

- Device tilt and orientation — which way is up, is the unit mounted in a vehicle
- Choosing north-up vs heading-up display mode
- Short dead-reckoning intervals between position updates
- Motion/shake detection

**For absolute heading, use the phone's GPS course-over-ground.** It is
magnetometer-derived, does not drift, and arrives free with the position already
being streamed. This is a further argument for keeping the phone in the loop
rather than adding a GNSS module: one link delivers both position and absolute
course.

## What the nav layer still needs

Built on top of `map_tiles`, which supplies only the basemap:

1. **Route ingest** — phone sends a decimated polyline + maneuver list once at
   trip start (a few KB). Cheap.
2. **Arc-length table** — cumulative distance along the polyline.
3. **Map matching** — project each position onto the polyline to get arc position
   `s`. This is what stops the marker wobbling onto the wrong road.
4. **Maneuver triggering** — `distToTurn = s_maneuver − s`, fired at 500/200/50 m.
5. **Off-route** — perpendicular distance from the polyline > 30–40 m for N s,
   with debouncing so one bad fix does not trigger a reroute.
6. **Route overlay** — project the whole polyline to screen space, draw as a single
   path, and let LVGL clip. **Do not clip per tile**; that is where the complexity
   and bugs live.
7. **ETA** — remaining arc length ÷ smoothed speed.

Roughly 800–1,500 lines. The hard parts are judgement, not code: map-matching
robustness, off-route debouncing, and maneuver hysteresis.

## Where BLE fits now

With maps resident on the SD card, the phone link is almost idle:

| Traffic | Rate | Size |
| --- | --- | --- |
| Position (packed binary) | 5–10 Hz | ~20 B |
| Route + maneuvers | once per trip | few KB |
| Commands (recentre, zoom, reroute) | occasional | tens of B |

**≈ 200 B/s sustained.** Compare with ~100 KB/s to stream tiles over BLE. The link
has enormous headroom, which is what makes it resilient to the jitter and dropouts
that a real phone-to-device BLE session will have.

Packed position payload: int32 lat + int32 lon (1e-7°), uint16 speed (cm/s),
uint16 COG (0.01°), uint8 satellites, uint32 timestamp = **18 bytes**.

## The round display

The 70.13 mm active circle is the inscribed circle of the 480×480 canvas; the
bezel covers the rest out to 95.86 mm. **The corners are physically hidden — no
software circular mask is required.**

Two things to handle:

- Centre the tile grid on the screen centre, so the marker path has margin.
- Reject touch events landing in the hidden corner region. The GT911 still reports
  a full 480×480 coordinate space.

## Principal risks

| Risk | Why it matters | Mitigation |
| --- | --- | --- |
| **LVGL frame rate at 480×480 with 9 PSRAM tiles** | **The reference only runs 640×172.** Compositing nine 128 KB PSRAM images at 480×480 is unproven. | **Test this first**, before writing BLE or nav code. Start at 3×3. |
| Grid centring jitter | Map jumps up to 128 px; forces constant SD reloads | Fork and fix (§ Bug, above) |
| LVGL 8 → 9 | Vendor demo is LVGL 8; component needs 9.3 | Write a fresh thin port; do not migrate the vendor glue |
| SD reload latency | 1-bit card, 128 KB per tile, no cache | Recentre rarely; add a cache in front of the component |
| Tile provenance | Public endpoints, author warns against bulk use | Move to a licensed tile provider before scale-up |
| Off-screen content | `map_tiles` centres on grid origin, not GPS | Fork; or place the GPS at grid centre by construction |

## Recommended build order

| Phase | Deliverable | De-risks |
| --- | --- | --- |
| 0 | Flash vendor test firmware | Board health |
| 1 | LVGL 9 display + touch port over vendor `esp_lcd` drivers | The version conflict; 480×480 RGB565 viability |
| 2 | `map_tiles` + 3×3 grid + marker driven by a hardcoded coordinate | **The real risk: frame rate at this screen size** |
| 3 | BLE position stream driving the marker | Link latency and jitter |
| 4 | Reproducible tile pipeline script | Reproducibility |
| 5 | IMU orientation + north-up/heading-up toggle | Needs phone COG for absolute heading |
| 6 | Route overlay + maneuver logic | The actual navigation feature |

**Phase 2 gates everything.** If LVGL cannot sustain an acceptable frame rate
compositing nine PSRAM tiles at 480×480, the approach needs rethinking — better
discovered at a bench than after a nav stack exists.
