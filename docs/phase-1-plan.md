# Phase 1 Plan — Offline Map + BLE Position + Pairing

Status: planned, not started.
Goal: ESP32 shows an offline map of Mumbai with a live marker from the phone. Phone pairs once via QR, then auto-reconnects.
Power: USB-powered, no battery in Phase 1. Ignore battery sense (GPIO4 floats without a cell); skip charge/battery UI.

Docs context: `docs/waveshare-2.8c/10-map-tiles-reference.md`, `02-pinout.md`, `03-display-touch-sensors.md`.

Repo layout: `firmware_v2/` (contract: `main/ble_service.h`), `app/` (mirror: `BleManager.kt`), `docs/`, `3mf/`. (v1 firmware, superseded, was deleted in the firmware_v2 rewrite.)

## Scope

In:
- ESP-IDF firmware: LVGL9 display, SD tiles, 3x3 grid, GPS marker, drag + pinch, BLE peripheral, QR pairing screen.
- Android app: QR scan, BLE central, bonded auto-reconnect via foreground service, 1–5 Hz fused location stream.
- Mumbai tiles, zoom 15–17, FAT32 SD.

Out:
- Routing, maneuvers, ETA, off-route, TTS/audio, IMU tilt UI, OTA, Wi-Fi runtime.

## Architecture

```
SD (/street_map/{z}/{x}/{y}.bin) → map_tiles (forked) → LVGL9 → ST7701S 480x480
Phone FusedLocation → BLE POSITION notify (18 B, 5 Hz) → marker + recenter
ESP32 QR (name+mac+token) → App scans → BLE connect + bond → NVS bonded → map
```

## Data contracts (v1, frozen for Phase 1)

BLE custom service, packed binary, no text. Propose UUIDs (replace with final):
- SVC `6E400001-...` (placeholder, generate one 128-bit UUID)
- `POSITION` notify, 18 B: int32 lat (1e-7 deg), int32 lon, uint16 speed cm/s, uint16 cog 0.01deg, uint8 sats, uint8 fix, uint32 phone_timestamp_ms
- `CMD` write: `0x01 recenter`, `0x02 set_zoom <u8>`, `0x03 ping`, `0x04 unpair`, `0x05 set_brightness <u8 0-100>`
- `STATUS` notify: tile errors, fps, link state (no battery field in Phase 1 — USB-powered)

QR payload (JSON, < 150 chars):
`{"n":"NAV-XXXX","m":"AA:BB:CC:DD:EE:FF","s":"<svc-uuid-short>","t":"<6-digit-token>"}`
App verifies token on first `CMD` write to prevent hijack.

MTU: negotiate 517, chunking N/A for Phase 1 (no JPEG/route yet).

## ESP32 firmware tasks

1. Scaffold
   - ESP-IDF 5.5.x, LVGL 9.3+, `0015/map_tiles` via `idf_component.yml` (fork, see #4).
   - Reuse vendor drivers: `ST7701S.c`, `GT911.c`, `TCA9554PWR`, `SD_MMC`, `I2C_Driver`, `BAT_Driver` from `docs/waveshare-2.8c/examples/esp-idf/main/`.
   - Delete `LVGL_Driver.c`, write `lvgl9_port.c` (~150 lines): `lv_display` flush via `esp_lcd_panel_draw_bitmap`, touch indev via GT911, tick via FreeRTOS timer.
   - sdkconfig: `SPIRAM_MODE_OCT`, `FLASHSIZE_16MB`, `DATA_CACHE_LINE_64B`, CPU 240 MHz. Grow `factory` to 4 MB (LVGL9 + map_tiles + NimBLE > 3 MB — check `idf.py size`).
   - Init order: `I2C → EXIO → LCD → Touch → SD → BLE → LVGL → map`. Never re-init LCD without `SD_Init()` after.

2. Display + touch bring-up
   - Backlight GPIO6 PWM ≥20 kHz, 18 MHz pclk first, 30 MHz only if stable.
   - GT911 reset dance (GPIO16 out-low → EXIO2 pulse → GPIO16 in). Verify falling-edge IRQ.
   - Acceptance: solid color, LVGL demo widget, touch coordinates correct.

3. SD + tiles
   - FAT32, 1-bit mode, `SD_Init()` after `LCD_Init()`.
   - Pipeline: `OfflineMapDownloader` rect Mumbai → `lvgl_map_tile_converter.py --jobs N` → copy `/street_map/{z}/{x}/{y}.bin` to SD.
   - Mumbai bbox v1: lat 18.85–19.35, lon 72.70–73.10, z15–17 (~4–5 GB). Skip z10–14, z18–19.
   - Acceptance: 9 tiles load from SD, no missing-file warnings for bbox center.

4. map_tiles fork (required)
   - Fix centering: GPS at grid center, not `floor(x)-1`. Recenter only when GPS leaves inner 1×1 tile.
   - Grid 3×3 (768×768, 1.13 MB PSRAM). Do not use 5×5+ yet.
   - Optional: 1-tile memory cache to avoid SD re-read on small pans.
   - Acceptance: marker static at fixed coordinate, pan 100 m causes no jump > 2 px and no SD reload storm.

5. Map UI
   - Center: Gateway of India 18.9219, 72.8347 default until first POSITION.
   - Marker: LVGL circle + outer ring, drawn in screen space.
   - Connection status icon: fixed top-center inside safe circle (480x480 round, avoid hidden corners). States: green solid = bonded + connected, amber blink 1 Hz = bonded + disconnected/searching, red/gray = unpaired. Driven by GAP events + POSITION age (>5 s without POSITION = stale, show amber). Tap icon → small popup with device name, RSSI, last-fix age.
   - Drag: single-touch drag pans map; on release, if marker outside inner region → reload grid.
   - Pinch: 2-point GT911 → zoom ±1 (z15–17), debounced 300 ms, reload.
   - Round screen: no mask needed (bezel covers corners). Reject touch in hidden corners.
   - Recenter button (touch corner) + zoom +/- buttons as fallback.
   - Brightness: backlight GPIO6 via LEDC PWM ≥20 kHz. `CMD 0x05` sets 0–100, applied immediately, persisted in NVS, restored on boot. Local brightness slider on device settings screen mirrors the same value. Clamp 5% minimum so screen never goes fully black from remote.
   - Acceptance: drag smooth, pinch changes zoom, marker stays geo-correct; icon reflects link state within 2 s of connect/drop; stale POSITION flips icon to amber; app slider → visible brightness change < 1 s; reboot restores last brightness.

6. BLE + pairing
   - NimBLE peripheral, advertise `NAV-XXXX` + SVC UUID. Bonding (Just Works), persist in NVS.
   - Unpaired boot → Pairing screen with QR (`nayuki/QR-Code-generator`, render to LVGL canvas ×6 scale) + name + instructions.
   - On bond + token verify → persist, switch to Map screen. Subsequent boots skip QR if bonded.
   - Unpair: long-press BOOT 5 s → erase bond, show QR. Also handle remote unpair via `CMD 0x04`: erase NVS bond, disconnect, return to QR screen, set status icon to unpaired.
   - POSITION handler: `gps_to_tile_xy`, update marker at up to 10 Hz, map-match = none in Phase 1 (raw marker).
   - Acceptance: cold boot unpaired shows QR < 3 s; scan → connected < 10 s; reboot auto-reconnects without scan; app Unpair → firmware returns to QR < 3 s.

## Android app tasks

1. Scaffold: Kotlin, minSdk 26, CameraX + MLKit/ZXing, `bluetooth-le`, `location`, `foreground-service` permissions. Target SDK per Play.
2. QR scan: parse `n/m/s/t`, store in EncryptedSharedPreferences, connect to MAC directly (no general scan for pairing).
3. BLE central: `BluetoothLeScanner` with SVC filter for auto-reconnect; `autoConnect=true` in foreground service; exponential backoff 5/15/60 s; bond via Just Works; write token to `CMD` on first connect.
4. Location: `FusedLocationProviderClient`, `PRIORITY_HIGH_ACCURACY`, 1 Hz while connected (5 Hz toggle in debug). Pack 18 B little-endian, write to POSITION via notify (phone is client → use write-without-response to ESP32 `POSITION` write char; or reverse roles — decide in implementation, keep 18 B format).
   - Note: simplest Phase 1 is phone as GATT client writing position to ESP32. Adjust contract line above accordingly; byte layout unchanged.
5. Minimal UI: status (connected/device, no battery in Phase 1), big Connect button, QR scan button, location permission rationale.
   - Brightness slider (0–100): writes `CMD 0x05` on change (throttle to 200 ms), reads back via `STATUS` on connect to sync position. Acceptance: drag slider → device brightness follows < 1 s.
   - Unpair button: sends `CMD 0x04`, then `removeBond()`, disconnects, clears stored MAC/token in EncryptedSharedPreferences, stops location stream, returns app to unpaired state. Requires confirm dialog (destructive). Acceptance: tap Unpair → both sides unpaired, ESP32 shows QR, app shows Scan prompt.
   - Service toggle: ON/OFF switch for foreground service. OFF stops BLE scan/connect, stops location updates, disconnects cleanly, removes persistent notification; ON restarts scan + auto-connect to bonded device. Persist toggle across restarts; default ON after first pairing. Acceptance: toggle OFF → no BLE/location within 5 s, no wakeups; toggle ON → reconnect < 20 s.
6. Acceptance: kill app → service restarts and reconnects in < 20 s (if toggle ON); walk 100 m → marker moves on ESP32 < 2 s lag; Unpair clears both ends; service OFF survives reboot stopped.

## Touch UX spec

- Single drag: pan. Release inside inner tile: no reload. Outside: reload grid centered on new GPS-projected center.
- Pinch in/out: zoom -1/+1 clamped z15–17. Show zoom toast.
- Tap marker: recenter. Corner buttons: + / - / ◎.
- Ignore touches with radius > 225 px from center (hidden corners).

## Test plan

- Bench: hardcoded lat/lon sweep → no jitter, SD reload count logged.
- Walk test (Mumbai): phone in pocket, ESP32 on USB power bank. Log POSITION rate, missed tiles, fps via `LV_USE_PERF_MONITOR`.
- Reconnect: toggle phone BT, expect auto-reconnect < 20 s.
- Power: measure USB current with backlight 100%/50% (sanity only, no battery-life target in Phase 1).

## Risks

- LVGL9 frame rate at 480×480 with 9 PSRAM images — Phase 2 gate. Mitigation: 3×3 only, bounce buffer, 18 MHz pclk fallback.
- GT911 pinch reliability — fallback to +/- buttons.
- BLE throughput/jitter — trivial at 200 B/s, but verify background kills on Samsung/MI.
- Tile size (~4 GB) + OSM bulk policy — personal use only, move to licensed tiles later.
- Vendor SD/LCD shared GPIO1/2 — any display sleep must re-run `SD_Init()`.

## Milestones

- M1: display + touch + QR screen (no map, no BLE). **Done 2026-09-25: builds clean, flashed, boots stable.** Verified over serial: I2C/LCD/Touch/GT911 (ID 911, cfg v71)/LVGL9 init OK, QR payload generated (`NAV-EB46`, token persisted across boots via NVS). Fixed along the way: LVGL8 include in vendored ST7701S, `lv_display_set_buffers` API name, Montserrat-20 Kconfig, 211 KB QR canvas moved to PSRAM (was overflowing `.dram0.bss`), LVGL loop moved to 12 KB GUI task (main-task stack overflow), LEDC 20 kHz reverted to vendor 5 kHz (unachievable at 13-bit). SD error in log is expected — no card inserted yet. Display itself not yet visually confirmed.
- M2: SD tiles + static Mumbai map + drag/pinch on hardcoded coordinate.
  **Browse mode done 2026-09-26**: QR screen has Skip button -> 3x3 offline map
  (MapTiler z15-17, 750 tiles on SD), drag-pan, pinch zoom, +/- buttons,
  recenter. Measured: tile shift ~350 ms (3 fresh loads @ ~118 ms/tile, 1-bit
  SD @ 40 MHz), render passes up to ~100 ms during pans. Responsive with mild
  choppiness at tile boundaries. Fixed along the way: GPT->MBR repartition,
  FATFS LFN on (STREET~1 bug), no-auto-format, slot rotation (was reloading
  all 9 per shift), accumulator cap (death spiral), single touch reader
  (invalidate-on-read starved LVGL), screen scroll pinned, manual drag deltas
  (vect reads 0 on non-scrollables). Follow-ups (deferred): async tile loader
  task, DOUBLE_FB direct mode, full Mumbai bbox download (~30k tiles).
- M3: BLE link + live marker from app.
- M4: bonded auto-reconnect + walk test. Phase 1 done.
- M3 status 2026-09-26: BLE live end-to-end. NimBLE peripheral advertising
  (NAV-EB46 visible), Just-Works bonding to NVS, POSITION (18 B) -> blue
  marker, CMD 0x01/0x02/0x04/0x05/0x06 handled, gray stale marker + persisted
  last fix across reboots, BOOT 5 s factory unpair, null-island guard both
  ends. Crash fixed (x2): (1) PAIR handler called show_map() in the NimBLE
  task while the GUI task renders -> LVGL heap corruption (backtrace decode +
  clean pair cycles after routing through GUI-task flags). Rule: NimBLE
  callbacks stash only, gui_task consumes. (2) Interrupt WDT on Core 0 with
  BT controller parked in semphr_take: USB-serial console BLOCKS when no
  reader is attached, and chatty INFO logs (flush rate, tile reloads,
  tile-missing bursts, NimBLE debug) wedged the BLE host task mid-pairing ->
  controller starvation -> WDT. Fix: ship at WARN verbosity (INFO/DEBUG
  compiled out via CONFIG_LOG_MAXIMUM_LEVEL=2). Verified with a 7-minute
  paired+streaming soak, zero crashes. Status dot removed; marker is the
  only link indicator (blue = live, gray = stale).

## Open questions

- GATT roles: phone-writes vs ESP32-notifies for POSITION — pick one in M1.
- Exact 128-bit service UUID.
- Token length / rotation policy.
