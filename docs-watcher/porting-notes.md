# Watcher vs Waveshare 2.8C — what the port took

**Status: implemented and flashed.** The port lives in
[`../firmware_watcher/`](../firmware_watcher/README.md). It builds, has been
backed up and flashed to a real Watcher, and boots: expander and power rails up,
SPD2010 panel taking colour frames over QSPI, LVGL buffers allocated, touch
initialised, no crash. What has *not* been confirmed is what the glass actually
shows — see that README's "Verified on hardware".

This document is the delta list and the reasoning behind it: what the two boards
share, what they don't, and which differences turned out to be load-bearing.
Quantified performance figures only exist for the Waveshare board so far.

## Same chip, different board

Both are ESP32-S3 with octal PSRAM. That is about where the similarity ends.
The Watcher is a *product* — power is switched by an I2C expander rail, the
panel is a different controller on a different bus, and the board carries a
second SoC and a camera.

| Concern | Waveshare 2.8C (current) | Watcher W1-A (target) |
| --- | --- | --- |
| Panel | ST7701S, **RGB565 parallel**, 480 × 480 | **SPD2010, QSPI**, 412 × 412 |
| Panel init | vendor init sequence over a dedicated SPI | `espressif/esp_lcd_spd2010` v1.0.1 (managed component) |
| Touch | GT911, I2C, own RST + INT GPIOs | SPD2010 (same part), I2C1, **INT via the expander** (`sensecap-watcher.h:97`) |
| IO expander | TCA9554PWR, 8-bit, own I2C | **PCA9535, 16-bit**, on the "general" I2C0 (addr `0x21`) |
| Expander owns | LCD CS/RST, touch RST, SD CS, buzzer | **all power rails**, knob button, SD detect, touch INT, Himax |
| SD | SDMMC 1-bit @ 40 MHz, shares GPIO1/2 with LCD | **SPI** on SPI2, shared with the Himax, CS 46, detect via expander |
| IMU | QMI8658 (gyro + accel) | **none** |
| RTC | PCF85063 (I2C) | PCF8563 (I2C, `0x51`) — *different part* |
| Flash | 16 MB | 32 MB |
| Audio | buzzer only | **ES8311 DAC + ES7243 ADC** — mic + 1 W speaker |
| Camera | none | OV5647 + **Himax HX6538** AI SoC (SPI2 + UART1) |
| Wake source | touch on **GPIO16** (RTC-capable) | expander `INT` on **GPIO2**, `ext0` level 0 (`sensecap-watcher.c:338`) |
| Free pins | **43 / 44 only** | Grove I2C + 2 GPIO + a USB-CDC "UART" on GPIO19/20 |
| Panel UI size | 480 × 480 | 412 × 412 |

## The five things most likely to cost a weekend, and what happened

**1. The partition table collides with factory data.**
`firmware_v2/partitions.csv` puts `nvs` at **0x9000, 0x6000**. The Watcher's
factory firmware puts **`nvsfactory` at 0x9000, 200 KB** — per-unit factory data
(and licence material). Flashing our table as-is would overwrite it.
**Resolved** by keeping the vendor table byte-identical, which lands our `nvs` at
0x3b000 and leaves `ota_0` at 0x110000 — verified from the built partition
binary. Back `nvsfactory` up anyway, and prefer `idf.py app-flash`.

**2. SPD2010 over QSPI is a different driver, not a different config.**
`lcd.c` drove ST7701S in RGB mode; the Watcher's panel hangs off SPI3 as quad
SPI with `dc_gpio_num = -1`. **Resolved** by using the managed
`espressif/esp_lcd_spd2010` component (2.0.0) with its QSPI template and the
vendor's 40 MHz clock. LVGL only needs the handle, so the change stayed inside
`lcd.c` — but see (3), because the *buffers* had to move too.

**3. LVGL 9 vs the vendor's LVGL 8.**
The vendor BSP is written against LVGL 8.4 / `esp_lvgl_port` 1.4 and returns
`lv_disp_t *`, so it can be read but not used. Our board layer is already ours,
so it was rewritten: `board.c`, `lcd.c`, `touch.c`, `sdcard.c`, `display.c` and
`main.c`'s init/sleep. The rest — BLE, tiles, map, media, call, boot, pairing —
is v2 unchanged.

**4. Power is gated by an expander, so init order is stricter.**
Not just ordering: `EXIO_PWR_SYSTEM` had to be raised before anything else and
dropping it powers the device off. **Resolved**, and the port improves slightly
on the vendor sequence by zeroing the output registers *before* taking the pins
as outputs, so no rail can be driven high by the PCA9535's power-on defaults.
Only the rails this firmware uses are raised — the Himax, codec, Grove and
battery-divider rails stay off.

**5. No IMU.**
Heading was never at risk: the phone already sends `cog` (course over ground,
`u16` in 0.01°) inside the 18-byte `POSITION` payload
(`firmware_v2/main/ble_service.h:192`). The real loss is only inertial
smoothing, tilt, and heading while stationary — none of which the nav path uses.

## Three things the port turned up that the docs did not

**A. An SPI interrupt / flash-cache hazard, which would have been an
intermittent crash.** Display flush completion is delivered from the SPI ISR,
which calls the flash-resident `on_color_trans_done()` →
`lv_display_flush_ready()`. IDF's `CONFIG_SPI_MASTER_ISR_IN_IRAM` defaults to
**y**, and an IRAM ISR *stays enabled* while a flash write has the cache
disabled — so the ISR would jump into flash with no cache and fault. A frame is
~17 ms and an NVS write a few ms, so the overlap is the common case. Fixed by
leaving the ISR in flash (`CONFIG_SPI_MASTER_ISR_IN_IRAM` not set), which makes
IDF mask it for the write and run the callback straight after. v2 never hit this
because its flush completion came from the RGB panel's own scan-out ISR and had
`SPIRAM_XIP_FROM_PSRAM` on.

**B. LVGL busy-spins without a flush wait callback.** `wait_for_flushing()` in
`lv_refr.c` falls back to `while(disp->flushing);` when no `flush_wait_cb` is
registered. With a 17 ms transfer that would spin the GUI task for the whole
frame. Fixed with a semaphore released by the ISR, with a timeout — the same
shape as v2's RGB `lcd_wait_frame()`.

**C. The panel is a hard performance ceiling.** 412×412 RGB565 is 339 KB, and
QSPI at 40 MHz over four data lines moves ~20 MB/s, so a full frame is **~17 ms
of pure bus time**. `map_view` does full-frame updates. This is the one place the
Watcher is clearly worse than the Waveshare board, and it is inherent to the
panel interface rather than fixable in software. First knob to turn is
`NAV_LCD_QSPI_MHZ`. (Still arithmetic — no fps numbers taken yet.)

**D. A bus-sizing mistake that looks exactly like a dead panel.** Found on the
first hardware boot. The panel's init commands went through, then *every* colour
frame failed to queue:

```
E lcd_panel.io.spi: panel_io_spi_tx_color(395): spi transmit (queue) color failed
E spd2010: panel_spd2010_draw_bitmap(698): send color failed
```

Cause: `max_transfer_sz` on the QSPI bus bounds both the maximum transaction
length *and* the number of DMA descriptors the driver reserves. Sizing it for a
whole frame (65,920 B) needs ~17 descriptors per transaction, and with the
component's default `trans_queue_depth` of 10 that overruns the descriptor pool —
so `spi_device_queue_trans()` fails on every frame. The fix is the vendor's own
sizing: a frame split into 16 chunks (21,218 B) with a queue depth of 1. The
intuitive "one transaction per frame" does not work. Written up in
[`../firmware_watcher/README.md`](../firmware_watcher/README.md#the-panel-bus-fix).

Also worth recording: `esp_io_expander_pca95xx_16bit` is **not** on the component
registry, so the expander is driven with raw `i2c_master` calls (four register
pairs) rather than a component. And `esp_lcd_touch_get_data()` returns `ESP_OK`
early when nothing is touched, so the caller must not assume it wrote the point
count.

## Corrected from the first pass

The wake story is worse than it looked, and it had been described optimistically.
The vendor's sleep mask drops `BSP_PWR_LCD`, which also removes power from the
touch controller — so while asleep **tapping the screen cannot wake the device**;
only the wheel button (or a charger state change) asserts the expander INT. That
is a genuine regression against the Waveshare build's tap-to-wake, and the
workaround (keeping the LCD rail up) costs sleep current.

## Notable carry-overs

- **NimBLE:** the vendor firmware is NimBLE too, with BLE buffers in PSRAM, MTU
  512 and blob transfer enabled (`sdkconfig.defaults`). Close to what
  `firmware_v2/main/ble_nav.c` already does — the BLE contract in
  `ble_service.h` should be portable unchanged, since it is a wire protocol, not
  a board detail.
- **Position arrives over BLE, not from an on-board receiver.** The phone's
  `FusedLocationProviderClient` is the only position source: an 18-byte
  little-endian `POSITION` payload — `i32 lat`/`i32 lon` in 1e-7°, `u16 speed`
  in cm/s, `u16 cog` in 0.01° (`firmware_v2/main/ble_service.h:192`). That means
  the board swap has **zero effect on the positioning path**, and the Watcher's
  meagre free-pin situation (2 GPIO + Grove I2C, plus an alarm UART whose pins
  Seeed do not document) is irrelevant to navigation. It also means the firmware
  stays a display end-point: no GNSS parsing, no UART pins, no antenna.
- **Deep sleep:** the *shape* is the same as ours — one RTC-capable line wakes
  the chip — but the line changes from touch GPIO16 to expander `INT` GPIO2.
  The Watcher also has a real power-off (rail drop) and a hardware power button,
  neither of which the Waveshare board has.
- **SD card:** still FAT32, still LFN-sensitive, still must not be
  auto-formatted. The bus changes from SDMMC 1-bit to SPI, and it is **shared
  with the Himax link** — so the Himax rail/idle behaviour matters even if we
  never talk to the camera. Tile reads are slower on SPI than on 1-bit SDMMC.
- **Flash headroom:** 32 MB / 12 MB app slots vs 16 MB. The map-tile storage
  story gets easier, not harder.

## One decision left open

`nvsfactory` (0x9000, 200K) still holds whatever Seeed wrote there — MAC, radio
calibration, possibly licence material. **Nothing in the port reads or writes
it**, and the partition table keeps our `nvs` clear of it, so this never has to
be decided in a hurry. It only matters if the image is ever flashed with a
partition table that shifts the region (check with `gen_esp32part.py` first) or
if the whole flash is erased. Back it up before the first flash and the question
can stay open indefinitely.

## Still open — needs eyes and a card, not more documents

The delta list above is closed, and the device now boots with the panel taking
frames and touch up. What remains is what tooling cannot see: **what is actually
on the glass** (colour order, mirroring, the scaled layouts at 412 px), touch
accuracy and the bezel mask, the SD card and `tiles.c` (no card was fitted), real
fps figures, and sleep/wake. Each is listed with what to look for in
[`../firmware_watcher/README.md`](../firmware_watcher/README.md#verified-on-hardware).

## Also in this folder

- [pinout.md](pinout.md) — the extracted GPIO + expander map, with file:line citations
- [hardware-overview.md](hardware-overview.md) — chips, connectors, serial console
- [firmware-sdk.md](firmware-sdk.md) — the vendor SDK, BSP API, partitions, Kconfig
- [wiki/](wiki/) — 35 wiki pages, raw markdown
- [hardware/](hardware/) — schematic, HX6538 and ESP32-S3 datasheets
- [MANIFEST.md](MANIFEST.md) — every file, with source URL and checksum
