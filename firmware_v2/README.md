# Firmware v2 — nav device (ESP32-S3-Touch-LCD-2.8C)

A from-scratch rewrite of `firmware/`. It keeps the **same BLE contract** (UUIDs,
POSITION/CMD/STATUS layouts, QR payload) and reads the **same SD tiles**
(`/sdcard/street_map/{z}/{x}/{y}.bin`). It also keeps the **same partition table
and NVS keys**, so the device name, pairing token, paired flag, phone bond and
last fix all carry over. No re-pairing is needed after flashing.

## Build / flash

```bash
source ~/.espressif/tools/activate_idf_v5.5.2.sh
cd firmware_v2
idf.py build
idf.py -p /dev/ttyACM0 flash
```

Dev aids (`idf.py menuconfig` → *Nav v2 options*; all off for normal use):
- `NAV_DEBUG_STATS`: prints fps, draw/wait ms, tile load times and heap every 5 s.
- `NAV_DEBUG_BLE`: logs connect, encryption and bond-store events, and dumps the bond/RPA table at boot.
- `NAV_DEBUG_DEMO_WALK`: feeds simulated 1 Hz fixes around the Gateway of India, so you can bench without a phone.

## Design (and what it fixes vs v1)

| v1 symptom | Cause | v2 |
| --- | --- | --- |
| Tearing / glitches | 1 frame buffer + partial-render copies | **3 PSRAM frame buffers**. LVGL renders straight into them (FULL mode, zero copies). The swap is synced to scan-out, and the GUI never stalls on vsync. |
| Glitches during NVS/bond writes | Flash writes disable the cache, which starves the bounce-buffer ISR | `SPIRAM_XIP_FROM_PSRAM` + `LCD_RGB_ISR_IRAM_SAFE` + `LCD_RGB_RESTART_IN_VSYNC` |
| Laggy pans (100 ms frames) | LVGL image pipeline + an opaque bottom layer filled under every frame | The map is blitted with tight row `memcpy` at `RENDER_START`, and a cover-check stops LVGL painting underneath. **28 ms/frame, the hardware floor** (a raw 480×480 PSRAM copy costs 26 ms on this board). |
| Tile hitches (~118 ms/tile, blocking) | SD reads into PSRAM fall back to one 512 B sector per transfer | A background loader on core 0 reads 32 KB chunks into internal DMA RAM. **~40 ms/tile**, never on the GUI task. |
| Reload storms | 9-slot grid | 28-slot LRU cache (3.6 MB PSRAM), prefetch ring, negative cache for missing tiles, center-first load order |
| Crashes (heap corruption, WDT) | Shared volatile flags across tasks, LVGL TLSF heap, NVS writes from the host task | Strict ownership: NimBLE callbacks only `xQueueSend`; the GUI task owns LVGL, touch and app NVS; libc malloc for LVGL |
| Re-pair failures | No `REPEAT_PAIRING` handling, no identity keys | Old bond deleted and pairing retried; ENC+ID keys exchanged both ways |
| "Pair again" popup loop | NimBLE bug: RPA records pile up (`ble_store_clear` and `ble_store_iterate` can't see them). Once the table is full, the stock handler unpairs the phone that just bonded | `ble_nav.c` purges orphaned RPA records at boot and on overflow, and never unpairs a phone to make room. `MAX_BONDS` raised to 5 |

Threading:

```
core 1  gui      LVGL, touch (single GT911 reader), screens, app NVS writes
core 0  NimBLE   GATT/GAP callbacks -> queues (position: overwrite-latest)
core 0  tile_ld  SD -> cache slot handed over in LOADING state
core 0  LCD ISR  bounce buffers (PSRAM FB -> internal SRAM -> panel)
```

Init order (load-bearing): NVS → I2C → TCA9554 → LCD (init SPI on GPIO1/2,
then released) → GT911 → SD (reuses GPIO1/2) → tiles → LVGL → BLE → GUI task.

## Files

| File | Role |
| --- | --- |
| `main.c` | Init order, BLE→GUI event plumbing, GUI loop, BOOT 5 s unpair |
| `ble_service.h` / `ble_nav.c` | BLE contract (mirrors `app/.../BleManager.kt`), NimBLE peripheral |
| `display.c` | LVGL 9 port: triple buffer, scan-out sync, touch indev |
| `lcd.c` | ST7701S init sequence + RGB panel |
| `touch.c` | GT911 on the new `i2c_master` driver |
| `board.c` | I2C bus, TCA9554 expander (shadowed outputs), backlight, BOOT button |
| `sdcard.c` | 1-bit SDMMC mount (40 MHz, falls back to 20 MHz), never formats |
| `tiles.c` | Tile cache + async loader |
| `map_view.c` | Map blit, marker (glides between fixes, heading cone when moving), drag/pinch, overlays |
| `ui_pair.c` | QR pairing screen (LVGL `lv_qrcode`) |
| `media_view.c` | Phone media page: round art, progress ring, volume arc, transport |
| `media_art.c` | Album art: BLE chunk reassembly → JPEG decode (`esp_jpeg`, ROM TJpgDec) → round mask → double buffer, in its own task |
| `ui_common.c` | Shared round button |
| `settings.c` | NVS, cached in RAM (same keys as v1) |

## Behaviour

- **Unpaired boot:** QR screen (or tap *Browse map*). **Paired boot:** map, centered on the last fix (grey) or the Gateway of India.
- **Marker:** blue while live; grey after 12 s with no fix. It glides between fixes. A heading arrow appears at ≥ 1 m/s and goes away below 0.5 m/s (the hysteresis stops creeping traffic from flickering it).
- **Heading-up:** in follow mode, while the arrow is shown, the map rotates so the arrow points up, easing with a ~200 ms time constant. Otherwise it eases back to north-up. Dragging leaves follow mode, so the map returns to north-up; drag and pinch account for the angle while it turns.
  - Rendering: north-up uses the row-`memcpy` blit (28 ms/frame). Rotated (`map_view.c`):
    - **Bilinear-filtered** (4-weight blend on spread RGB565), only inside the round panel's disc.
    - Drawn in 32×32 blocks shared by both cores: the GUI task plus a core-0 helper running below the tile loader.
    - Each block first copies the source rectangle it samples into internal SRAM, then samples from there. Sampling straight from PSRAM stalled on cache misses, since the bus is shared with LCD scan-out.
    - Measured at every heading: nearest-neighbour 26 fps (pixelated), bilinear 1 core 12 fps, 2 cores 18 fps, **2 cores + SRAM staging 22–23 fps (42–46 ms/frame)**.
- **Status pill:** Live / Waiting for GPS / GPS signal lost / Searching for phone / Not paired.
- **Gestures:** drag to pan (leaves follow mode, shows the ◎ recenter button); pinch to zoom ±1, anchored at the pinch; +/− buttons. The zoom range is auto-detected from the card.
- **Commands:** `0x05` brightness applies instantly (5 % floor) and is persisted once the slider settles.
- **Unpair:**
  - App Unpair (`0x04`): deletes only that phone's bond (the app removes its own side), then shows the QR.
  - BOOT held 5 s, or the status pill held 15 s (countdown toast after 1.5 s; release to cancel): **keeps bonds** and issues a new pairing token (the old QR no longer pairs), drops the link, and shows the QR. Never erase a bond the phone still holds: Android then loops on "pair again".
- **App QR scan:** removes any old system bond for that MAC before connecting, so pairing always starts clean.

## Media controls

The 🎵 button on the map (left) opens the media page; ◀ (same spot) goes back.
The page has round album art, a progress ring, a draggable volume arc, volume +/− on the right
(where zoom is on the map), and prev / play-pause / next.

BLE extension (existing UUIDs unchanged, mirrored in `BleManager.kt` / `MediaBridge.kt`):

| Characteristic | UUID | Direction | Payload |
| --- | --- | --- | --- |
| MEDIA_INFO | `790d1d86-2191-404d-a0f5-0e960990e190` | phone → device, write-no-rsp | `0x01` STATE (13 B): flags (playing/session/no-access), volume %, u32 position ms, u32 duration ms, u16 art id. `0x02` ART: u16 id, u32 offset, u32 total, data = 200×200 baseline JPEG ≤ 48 KB. `0x03` TEXT (on track change): title, NUL, artist |
| MEDIA_CMD | `f8a580f6-aa84-4898-ad67-a66c12262ac1` | device → phone, notify | `1` play/pause, `2` next, `3` prev, `4` vol+, `5` vol−, `6` set volume (u8 %), `7` resync |

- STATE is sent only on changes; the device advances the progress ring itself.
- Title and artist show above the album art, and in a toast over the map when the track changes. The board font is Latin-only, so `media_view.c` drops non-Latin characters instead of drawing empty boxes; long titles are ellipsised (toast: clipped to 37 chars).
- The map's overlay buttons are translucent white with a drop shadow, so they read as floating controls.
- If art for the current track is missing or stalls for 3 s, the device sends a resync.
- **GATT cache:** the device sends Service Changed on every boot, so bonded phones re-read the characteristic table after firmware updates. As a fallback, the app clears Android's GATT cache (`refresh()`) once if the media characteristics are missing.
- **App requirement:** Notification access, needed by Android to read other apps' media sessions. The *Enable media controls* button opens the setting.
- **Art sources:** metadata bitmap, `content://` URIs, or `https://` URLs. Media3-based players publish the URL first and the bitmap later; each new source is tried until one works.

## Measured on hardware (2026-10-02)

- Continuous scrolling: **33 fps** sustained for 60 s while streaming 185 tiles (draw 28 ms, vsync wait 0 ms). Heap flat.
- Tile load: 38–41 ms average, 55 ms max.
- BLE with a PC central: discovery, all characteristics, STATUS, PING, POSITION stream, zoom, brightness, wrong-token rejection, PAIR → map transition, and 6/6 connect/disconnect cycles (~2–3 s each).
- Bond lifecycle (PC as the phone): fresh pair, reconnect, reboot + reconnect, app unpair, re-pair, phone-forgot-bond re-pair: all pass.
- Real phone (Nothing Phone 3, Android 17): QR scan → clean pair (stale bond auto-removed) → Live; board reboot → automatic encrypted reconnect, no prompt.
- Media (Nothing Phone 3, ViMusic): art 5–23 KB per track, decoded in 41–82 ms off the GUI task; play/pause, next/prev, volume arc/buttons and progress all confirmed working.
- Shipping build is silent on the console (WARN level).
