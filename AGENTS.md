# AGENTS.md — nav-esp32

ESP32-S3 round-display nav device + Android companion app. Monorepo.

**Docs first.** `docs/` is the source of truth — read it before touching code:
- `docs/phase-1-plan.md` — what we're building, milestones, BLE contract
- `docs/waveshare-2.8c/02-pinout.md` — the full GPIO map (extracted, not on wiki)
- `docs/waveshare-2.8c/10-map-tiles-reference.md` — map component assessment
- `docs/README.md` — index. If confused, read docs before guessing.

## Layout

- `firmware/` — ESP-IDF 5.5.2, LVGL 9.3, NimBLE. Build: `source ~/.espressif/tools/activate_idf_v5.5.2.sh && cd firmware && idf.py build && idf.py -p /dev/ttyACM0 flash`
- `app/` — Kotlin, Gradle wrapper 9.3.1 (cached), AGP 8.13.2. Build: `cd app && ./gradlew :app:assembleDebug`. Java 25 only (Java 21 install is a stub).
- `tools/` — `download_tiles.py` (MapTiler, polite), `make_test_tiles.py`
- `tmp/` — tile staging, gitignored. Never commit tiles or binaries.
- `docs/waveshare-2.8c/examples/` — vendored vendor demo sources (reference only)

## Rules that cost weekends when broken

1. **sdkconfig lies by omission.** An explicit `# ... is not set` in `sdkconfig` overrides `sdkconfig.defaults`. When adding a Kconfig option, flip it in BOTH files.
2. **Ship at WARN log verbosity.** The USB-serial console BLOCKS when no reader is attached; INFO floods (flush rate, tile reloads, NimBLE debug) wedge the BLE host task and trip a Core 0 interrupt WDT. `CONFIG_LOG_DEFAULT_LEVEL_WARN=y` + `MAXIMUM=2`. Never ship INFO in hot paths.
3. **NimBLE callbacks stash only.** GATT/SM callbacks run in the host task: set volatile flags, return. The GUI task consumes. LVGL calls or NVS writes from the host task corrupt/freeze (proven twice via backtrace decode).
4. **All NVS writes run in the GUI task.** Same reason as (3) — flash + BLE controller contention.
5. **Init order is load-bearing:** I2C → EXIO → LCD → Touch → SD → BLE → LVGL → map. LCD before SD always (shared GPIO1/2); any LCD re-init must be followed by `SD_Init()`.
6. **Five signals hide behind the TCA9554 expander** (LCD RST/CS, touch RST, SD CS, IMU INTs, buzzer). `EXIO_Init()` after I2C, before everything else. Only **two free GPIOs exist: 43/44**.
7. **SD card requirements:** FAT32 + MBR partition table (GPT fails mount with FR_NO_FILESYSTEM=13), LFN enabled (`CONFIG_FATFS_LFN_HEAP=y`, else `street_map` reads as `STREET~1`), `format_if_mount_failed=false` (never auto-format user data). 1-bit bus @ 40 MHz.
8. **BLE contract is mirrored:** `firmware/main/ble/ble_service.h` ↔ `app/.../BleManager.kt`. UUIDs are fixed — generated once, byte order is reverse-string (see `ble_uuid_to_str`). Change both sides together.
9. **Touch: single reader only.** `esp_lcd_touch get_coordinates` invalidates the sample — a second reader starves LVGL (press/release strobing, frozen drags). LVGL indev reads; everyone else uses `touch_last_sample()`.
10. **LVGL 9, never 8.** Vendor demo is LVGL 8; we wrote a fresh port (`display/lvgl9_port.c`). `lv_display_set_buffers` (not `set_draw_buffers`).

## Debugging

- Serial: `python3 -c` + pyserial, or `idf.py monitor`. Note: native USB re-enumerates on reset and kills handles; `esptool run` (soft reset) preserves them.
- Decode crashes: `xtensa-esp32s3-elf-addr2line -e firmware/build/nav-esp32-fw.elf -f -C <addrs>`. Distinguish Interrupt WDT (starved CPU, backtrace shows victim) from Task WDT and from TLSF heap corruption (use-after-free elsewhere).
- ModemManager grabs ttyACM — port-busy errors are usually an orphaned watcher (`fuser /dev/ttyACM0`, kill it), not hardware.
- adb over Wi-Fi: `adb pair` + `adb connect`. Phone and PC on same Wi-Fi.

## Secrets

- The MapTiler API key lives ONLY in shell invocations (`tools/download_tiles.py --url ...?key=KEY`). Never commit it, never put it in a file. Tile PNGs/bins stay in `tmp/` (gitignored).
