# Firmware — nav device (ESP32-S3-Touch-LCD-2.8C)

ESP-IDF + LVGL 9 + SD map tiles + BLE peripheral. See `docs/phase-1-plan.md`.

## Layout

```
firmware/
  CMakeLists.txt            project root
  partitions.csv            nvs / factory (4 MB) / storage
  sdkconfig.defaults        16 MB flash, 240 MHz, LVGL perf monitor
  sdkconfig.defaults.esp32s3  octal PSRAM 80 MHz
  main/
    main.c                  entry: init order I2C→EXIO→LCD→Touch→SD→BLE→LVGL→map
    CMakeLists.txt
    idf_component.yml       lvgl >=9.3, map_tiles fork (to be vendored)
    Kconfig.projbuild       DOUBLE_FB / bounce / tear-sem options
    ble/                    GATT service (POSITION/CMD/STATUS), bonding, QR token
    display/                LVGL9 port over esp_lcd + GT911 indev + backlight PWM
    map/                    map_tiles fork glue: 3x3 grid, centering fix, marker
    ui/                     pairing QR screen, map screen, status icon
    drivers/                vendored vendor drivers (ST7701S, GT911, TCA9554, SD, IMU, RTC)
  components/
    README.md               map_tiles fork goes here (not committed yet)
```

## Vendor drivers

Copy from `docs/waveshare-2.8c/examples/esp-idf/main/` into `main/drivers/`,
except `LVGL_Driver/` which is LVGL8 — replaced by `main/display/lvgl9_port.c`.

Init order matters: LCD before SD; EXIO after I2C, before LCD/Touch/SD.
Shared GPIO1/2 (LCD SPI vs SD): any LCD re-init must be followed by `SD_Init()`.

## BLE contract (Phase 1)

- Service UUID: TODO (generate 128-bit, replace placeholder in `main/ble/ble_service.h`)
- POSITION: 18 B packed (lat/le32 1e-7, lon, speed cm/s u16, cog 0.01deg u16, sats u8, fix u8, ts ms u32)
- CMD: `0x01 recenter`, `0x02 set_zoom <u8>`, `0x03 ping`, `0x04 unpair`, `0x05 set_brightness <u8 0-100>`
- QR payload: `{"n","m","s","t"}` — rendered with QR-Code-generator, ×6 scale.

## Build

```bash
cd firmware
idf.py build
idf.py flash monitor
idf.py size   # watch factory 4 MB ceiling
```
