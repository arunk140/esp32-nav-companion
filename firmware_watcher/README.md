# Firmware — nav device (SenseCAP Watcher W1-A)

The nav-esp32 companion firmware ported from the Waveshare ESP32-S3-Touch-LCD-2.8C
to the **Seeed Studio SenseCAP Watcher W1-A**. Same product behaviour, same BLE
contract, same map: a different board underneath.

> **Status: flashed and running on hardware.** The image boots on the Watcher:
> the SPD2010 panel initialises and takes colour frames, the PCA9535 expander and
> its power rails come up, LVGL allocates both PSRAM buffers, touch initialises,
> and it stays up with no crash, no WDT and no transfer errors.
>
> **Checked by serial log, not by eye.** The tooling cannot see the panel, so
> what is on the glass is only known from what the owner reports. A first round
> of fixes from that (colour byte order, touch, sleep, 412 px layout) is in; see
> [Verified on hardware](#verified-on-hardware) for what was and was not checked.
>
> The first flash also turned up a real bug in the panel bus configuration; it is
> written up in [The panel bus fix](#the-panel-bus-fix) because it is the kind of
> thing that looks like a dead panel.

## Build / flash

```sh
source ~/.espressif/tools/activate_idf_v5.5.2.sh
cd firmware_watcher
idf.py build

# FIRST TIME, and before any flash: back up the factory data.
# `nvsfactory` (0x9000, 200K) is per-unit: MAC, radio calibration, possibly
# licence keys. Nothing here writes it, but a wrong offset would destroy it.
esptool.py --chip esp32s3 --baud 2000000 read_flash 0x9000 204800 nvsfactory.bin

# app-flash, not flash: writes only the app slot (ota_0 @ 0x200000) and
# leaves the partition table, bootloader and nvsfactory alone.
idf.py -p /dev/ttyACM0 app-flash
idf.py -p /dev/ttyACM0 monitor
```

Use the **bottom** USB-C port — the one on the back is power only. Two UARTs
enumerate (one ESP32-S3, one Himax) and the order is not fixed, so if the monitor
is silent, try the other port.

## The unit on the bench

| | |
| --- | --- |
| Port | `/dev/ttyACM1` (the second interface of the QinHeng dual-serial bridge; `ttyACM0` is the Himax) |
| Chip | ESP32-S3 rev v0.2, 8 MB PSRAM, 32 MB flash, quad flash set in eFuse |
| MAC | `8c:bf:ea:09:b4:60` |
| Fuses | **no** flash encryption, **no** secure boot, no fused key — a plaintext image boots normally |
| `nvsfactory` | **empty** on this unit (every byte 0xFF) |

### Backup before flashing

A full 32 MB image was read off the device before the first write and lives in
`tmp/watcher-backup/` (gitignored — **copy it somewhere durable**, `tmp/` is
scratch):

| File | Size | sha256 |
| --- | --- | --- |
| `factory_full_32MB.bin` | 33554432 | `b40e37b5d364160c061b9067ec7fd7724829cd6de082d1a6218bfb53de4c979f` |
| `nvsfactory.bin` | 204800 | `bde1ad06f099691704c1fee05bc1bda810539d8cb1aefd5ea6fcdf77b4a1be0f` |
| `pt_ondevice.bin` | 4096 | the partition table as read from the device (used to verify ours matches) |

To put the factory firmware back:

```sh
esptool.py --chip esp32s3 --port /dev/ttyACM1 -b 2000000 \
  write_flash --flash_size 32MB 0x0 tmp/watcher-backup/factory_full_32MB.bin
```

That restores everything including the bootloader and the vendor's app slots.

### Reading and writing speed

The CH34x bridge honours the baud rate on its UART side, so it matters:
`-b 2000000` gives ~109 KB/s (32 MB in ~5 min), while esptool's 460800 default
crawls at ~13 KB/s (40 min). Seeed's own instructions use 2 Mbaud. `idf.py` needs
the explicit path here (`python $IDF_PATH/tools/idf.py`), because the activation
script sets `IDF_PATH` but does not put `$IDF_PATH/tools` on `PATH`.

## What changed vs firmware_v2

| | Waveshare 2.8C | Watcher W1-A | Files |
| --- | --- | --- | --- |
| Panel | ST7701S, RGB565 parallel, 480×480 | SPD2010, **QSPI**, 412×412 | `lcd.c` rewritten |
| Panel bring-up | Vendor init sequence over a dedicated SPI | `espressif/esp_lcd_spd2010` (managed component) | `lcd.c` |
| Frame delivery | LVGL wrote into the panel's own triple buffers; flush switched scan-out | LVGL keeps two PSRAM buffers; flush is a **DMA push**, completion from the panel IO ISR | `display.c` |
| Touch | GT911, raw I2C register protocol | SPD2010 via `espressif/esp_lcd_touch_spd2010` | `touch.c` rewritten |
| I2C buses | One shared bus (touch + expander) | **Two**: I2C0 general (expander, RTC) and I2C1 (touch) | `board.c`, `touch.c` |
| IO expander | TCA9554PWR, 8-bit, pins only | **PCA9535, 16-bit, and it switches the power rails** | `board.c` rewritten |
| SD card | 1-bit SDMMC @ 40 MHz, shared GPIO1/2 with the panel's init SPI | **SPI** on SPI2 @ 20 MHz, shared with the Himax link | `sdcard.c` |
| Wake from sleep | Touch INT, GPIO16 (`ext1`) | Expander INT, GPIO2 (`ext0`) | `main.c` |
| Long-press button | BOOT button (GPIO0) | Wheel press switch, behind the expander | `board.c` |
| IMU | QMI8658 | none | — |
| Flash | 16 MB, `factory` app | **32 MB**, vendor `ota_0`/`ota_1` table | `partitions.csv` |
| Screen layout | 480×480 | 412×412, scaled via `UI_SC()` | all views |

Everything else is v2 unchanged: `ble_service.h`/`ble_nav.c`, `tiles.c`,
`map_view.c`'s rendering core, `media_view.c`, `nav_view.c`, `call_view.c`,
`boot_view.c`, `media_art.c`, `settings.c`, `ui_pair.c`, `ui_common.c`.

**The GPS path is untouched.** Position arrives over BLE from the phone's fused
location provider, so the board swap has no effect on navigation — and because
there is no on-board receiver, the Watcher's pin shortage does not matter.

## Files

| File | Role |
| --- | --- |
| `board.c` | I2C0/I2C1, **PCA9535 expander + power rails (raw I2C, no component)**, backlight on GPIO8, wheel button and rotation (pulse counter on GPIO41/42), battery sense on GPIO3, pin latching for sleep |
| `lcd.c` | SPD2010 QSPI panel on SPI3 |
| `touch.c` | SPD2010 touch on I2C1 |
| `sdcard.c` | SD over SPI2, 20 MHz → 10 MHz fallback, never formats |
| `display.c` | LVGL 9 FULL mode, two PSRAM draw buffers, byte swap to the panel's big-endian RGB565, QSPI flush |
| `main.c` | Init order, BLE→GUI plumbing, GUI loop, deep sleep via expander INT |
| *(the rest)* | Unchanged from `firmware_v2/` — see `firmware_v2/README.md`, which documents the BLE contract, map, media and call behaviour in full |

## Two things in sdkconfig that are load-bearing

**1. `# CONFIG_SPI_MASTER_ISR_IN_IRAM is not set`.** Flush completion is
delivered from the SPI ISR, which calls the flash-resident
`on_color_trans_done()` → `lv_display_flush_ready()`. IDF's default puts that ISR
in IRAM, where it *stays enabled while a flash write has the cache disabled* —
so the ISR would jump into flash with no cache and fault. Leaving the ISR in
flash means IDF masks it for the duration of the write and the callback runs
straight afterwards. A frame takes ~17 ms and an NVS write a few ms, so that
overlap is the common case, not a corner case. If it is ever turned back on,
every function in that ISR's chain — including LVGL's flush path — must be
IRAM-safe.

**2. `CONFIG_SPIRAM_XIP_FROM_PSRAM` is off** (v2 has it on). v2 needed code in
PSRAM so a flash write could not stall the RGB panel's scan-out, which ran
continuously from those buffers. There is no scan-out here: the panel is pushed
over QSPI. The vendor's own sdkconfig does not set it either.

`display.c` also sets a `flush_wait_cb` on purpose. LVGL's `wait_for_flushing()`
falls back to `while(disp->flushing);` when no wait callback is set, which would
busy-spin the GUI task for the whole transfer; ours blocks on a semaphore the ISR
releases.

## The panel bus fix

Found on the first hardware boot; worth reading before touching `lcd.c`.

**Symptom.** The panel's init commands went through and the driver reported
`SPD2010 up: 412x412 QSPI @ 40 MHz`, but every *colour* frame failed:

```
E lcd_panel.io.spi: panel_io_spi_tx_color(395): spi transmit (queue) color failed
E spd2010: panel_spd2010_draw_bitmap(698): send color failed
W display: panel transfer did not complete in 100 ms
```

Init commands are 8 bytes; a frame is 339 KB. So the failure was size-dependent,
on the queue step — `spi_device_queue_trans()` returning an error.

**Cause.** The QSPI bus's `max_transfer_sz` bounds *both* the maximum
transaction length and how many DMA descriptors the driver must reserve. Setting
it to a whole frame (65,920 B) needs ~17 descriptors per transaction, and with
the component's default `trans_queue_depth` of 10 that is far past the pool. The
queue call then fails with "no available dma descriptors" for every frame, which
reads as a dead panel rather than a bus-sizing mistake.

**Fix.** Take the vendor's numbers, which are the ones known to work on this
glass: split a frame into 16 chunks and queue one transaction at a time.

```c
#define LCD_DMA_SIZE_DIV 16   // 412*412*2/16 = 21218 bytes per transaction
#define LCD_TRANS_QUEUE_DEPTH 1
```

i.e. the vendor's `CONFIG_BSP_LCD_SPI_DMA_SIZE_DIV=16` and
`CONFIG_BSP_LCD_PANEL_SPI_TRANS_Q_DEPTH=1`. "One transaction per frame" is the
intuitive choice and it does not work.

## Verified on hardware

Confirmed on a real Watcher (MAC `8c:bf:ea:09:b4:60`, ESP32-S3 rev v0.2,
32 MB flash, quad flash in eFuse, **no** flash encryption and **no** secure
boot):

| | |
| --- | --- |
| Bootloader + partition table + otadata + app flash | verified write, hashes matched |
| Boots and stays up | yes — no crash, no WDT, no backtrace over repeated 35 s captures |
| Expander + power rails | yes — `rails up: LCD \| SD \| SYSTEM`, and the SD-detect input reads back |
| SPD2010 panel init over QSPI | yes — `SPD2010 up: 412x412 QSPI @ 40 MHz` |
| Panel colour transfer | yes — no queue failures and no timeouts after the bus fix |
| LVGL 9 + both PSRAM draw buffers | yes — `display up: 412x412, 2 full buffers in PSRAM` |
| Touch (SPD2010 on I2C1) | yes — init reported no error |
| Bootloader/app version pairing | ours (IDF 5.5.2) written, boots |
| `nvsfactory` | **empty on this unit** (all 0xFF), and untouched |

Still unverified, and the honest list of what to look at first:

1. **What is actually on the glass.** The first build sent LVGL's little-endian
   RGB565 to a panel that wants it high byte first (the vendor sets
   `LV_COLOR_16_SWAP`); `display.c` now swaps in `flush_cb`. If red and blue are
   still exchanged, change `LCD_RGB_ELEMENT_ORDER_RGB`; if the image is rotated
   or flipped, `esp_lcd_panel_mirror()` and the touch `swap_xy`/`mirror_*` flags.
2. **The scaled layouts.** Screens are proportionally scaled from a 480×480
   design via `UI_SC()`; text sizes are not scaled, so expect a few labels to
   want hand-tuning at 412.
3. **Touch.** The SPD2010 only returns points when it has a new report, and
   reports a lift as a zero-weight point; `touch_poll()` now holds the last
   sample between reports and drops zero-weight points. Written from the driver
   source, not from captured touch data. Accuracy and the 98 % bezel mask are
   also unchecked.
4. **SD card.** A card mounts over SPI at 20 MHz (card-detect is active low; the
   first build read it inverted and logged "no card" with one fitted). The card
   on the bench has no `/sdcard/street_map`, so `tiles.c` and the tile map are
   still untested.
5. **Performance.** No fps/draw/xfer numbers yet; the ~17 ms/frame QSPI ceiling
   is still theory. `CONFIG_NAV_DEBUG_STATS` (plus `NAV_DEBUG_DEMO_WALK` for
   movement without a phone) is the way to measure it.
6. **Sleep and wake.** The device now stays asleep (it used to wake at once:
   see [Power and sleep](#power-and-sleep)). Waking with the wheel button has
   not been tried; only a reset over USB has.
7. **Himax rail off while the SD shares SPI2** — still untested, needs a card.
8. **`FLASH_MODE_AUTO_DETECT`.** The vendor forces QIO and this port leaves
   auto-detect on; the boot log shows the ROM stage starting in `DIO`.

## Partitions

`partitions.csv` tracks **the table actually on the device**, read back with
`esptool.py read_flash 0x8000 0x1000` — which is *not* the table in the vendor
SDK's `examples/factory_firmware/partitions.csv`:

| | vendor SDK example | the device |
| --- | --- | --- |
| `ota_0` | 0x110000, 12 M | **0x200000, 4 M** |
| `ota_1` | 0xd10000, 12 M | **0x600000, 4 M** |
| trailing | `model` 1 M + `storage` 6080 K | **`assets` 16 M SPIFFS** |

Flashing the SDK's table would have put `ota_1` in the middle of the device's
16 MB `assets` partition. The device wins.

Our generated table is **byte-identical** to the one already in flash (verified
with `cmp`), which is what keeps two things true:

* our `nvs` is at **0x3b000**, immediately after `nvsfactory` (0x9000 + 200K), so
  the two never overlap;
* `ota_0` is at **0x200000**, so `app-flash` lands exactly where the bootloader
  looks.

```
nvsfactory,data,nvs,0x9000,200K
nvs,data,nvs,0x3b000,840K
otadata,data,ota,0x10d000,8K
phy_init,data,phy,0x10f000,4K
ota_0,app,ota_0,0x200000,4M
ota_1,app,ota_1,0x600000,4M
assets,data,spiffs,0xa00000,16M
```

Because `idf.py` derives the `app-flash` address from this file, the file must
keep matching the device. If it is ever regenerated from the SDK example, the app
lands where nothing will boot it. Re-check with:

```sh
esptool.py --chip esp32s3 --port /dev/ttyACM1 read_flash 0x8000 0x1000 pt.bin
gen_esp32part.py pt.bin
```

`assets` is the vendor's 16 MB SPIFFS region; this firmware does not mount it and
never writes it. Our app is ~1.3 MB in a 4 MB slot.

## Power and sleep

Idle behaviour is v2's: screen off after `NAV_SCREEN_OFF_MS`, then deep sleep
after `NAV_DEEP_SLEEP_MS`, and never while BLE is connected. What differs:

* before sleeping, the peripheral rails are dropped (LCD, SD, AI, codec, Grove,
  battery divider) — `EXIO_PWR_SYSTEM` stays up, so the device stays alive on its
  backup battery;
* the wake source is the expander's shared interrupt on **GPIO2** (`ext0`, active
  low, RTC pull-up);
* because the sleep drops the LCD rail, **the touch controller is unpowered while
  asleep, so tapping the screen cannot wake it** — the wheel button (or a charger
  state change) is what wakes the device. That is a real change from the
  Waveshare build's tap-to-wake. To experiment, keep `EXIO_PWR_LCD` up in
  `board_power_rails_off()` and measure the sleep current.

Two things `sleep_now()` has to do on this board, both found on hardware:

* **Clear the expander interrupt last.** The wake line asserts on any input
  change and stays asserted until the inputs are read. Dropping the rails
  changes several inputs, and so does releasing the wheel after the long press,
  so without a final read the chip wakes the instant it sleeps.
* **Latch the backlight and panel bus pins low** (`board_sleep_pins_hold()`).
  Left floating in deep sleep, the backlight enable can turn the backlight on
  and the bus lines feed the unpowered panel, which shows as flickering lines.
  The latch survives the wake reset; `board_sleep_pins_release()` undoes it
  first thing at boot.

There is no IMU, so nothing is lost to inertial input: heading comes from the
phone's course-over-ground field in the 18-byte `POSITION` payload.

## Performance expectations

The panel is the one place this board is worse than the Waveshare one. A full
412×412 RGB565 frame is 339 KB, and QSPI at 40 MHz over four data lines moves
~20 MB/s, so **a frame costs ~17 ms of pure bus time**. That is a hard ceiling
around 60 fps, and in practice the usable rate is lower once rendering is added.
`map_view` does full-frame updates, so this is the number to watch.

Mitigations if it proves too slow, in order of preference: raise
`NAV_LCD_QSPI_MHZ` (the component defaults to 20, the vendor runs 40, and the
part may take more — test it); then reduce the work per frame. Tiles themselves
are read once and cached, and tile loads happen on core 0.

This is still arithmetic, not measurement: the port now boots and drives the
panel, but no fps figures have been taken. `CONFIG_NAV_DEBUG_STATS=y` logs
`fps / draw / xfer` every 5 s (add `CONFIG_NAV_DEBUG_DEMO_WALK=y` to move without
a phone), which is the quickest way to replace the estimate with a number.

SD is also slower in principle: SPI at 20 MHz against 1-bit SDMMC at 40 MHz. Tiles
are cached, so this shows up as slower first-paint per tile rather than steady
state.
