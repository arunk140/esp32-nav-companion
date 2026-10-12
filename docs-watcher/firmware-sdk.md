# SenseCAP Watcher — firmware SDK reference

Notes on the vendor SDK, what is vendored locally, and the facts that matter
for a port. Everything here is derived from the actual sources.

- Upstream: <https://github.com/Seeed-Studio/SenseCAP-Watcher-Firmware>
- Licence: Apache-2.0
- Last upstream commit at time of writing: 2026-07-28

## What is vendored, and what is not

`firmware-sdk/` is a **subset** of the SDK, kept small on purpose:

| Path | Why |
| --- | --- |
| `components/sensecap-watcher/` | The BSP — **the board facts**: pins, display, touch, expander, power, audio, SD. This is the file the [pinout](pinout.md) is extracted from. |
| `components/knob/` | Rotary encoder driver used by the wheel |
| `components/esp_io_expander_pca95xx_16bit/` | The 16-bit expander driver (with the `_ex` variant the BSP calls) |
| `examples/get_started/` | Minimal correct init order for the board |
| `examples/lvgl_demo/`, `examples/knob_rgb/` | Small UI/input references |
| `examples/factory_firmware/docs/` | The product developer guides (also mirrored on the wiki) |
| `examples/factory_firmware/{README,CHANGELOG,version.txt,partitions.csv,sdkconfig.defaults}` | Facts about the shipped firmware |

Vendored **source only** — no binaries or demo assets. `examples/get_started/`
lost its generated `main/image.c` (a 1 MB embedded JPEG array) and its
`spiffs/` demo assets (`animation.gif`, `image.jpg`, `hello.txt`) for that
reason, so its `main/CMakeLists.txt` still names `image.c` and the example will
not build as vendored. That is deliberate: the file worth reading there is
`get_started.c`, for the board init order. To actually build it, take it from
upstream.

**Not vendored** — fetch a full clone if you need them:

```sh
git clone --depth 1 https://github.com/Seeed-Studio/SenseCAP-Watcher-Firmware \
  tmp/watcher-sdk   # ~430 MB with submodules omitted
```

- `components/lvgl/` (~99 MB) — **LVGL 8.4.0**, not 9. Our firmware targets
  LVGL 9.6, so this is reference only.
- `components/esp_jpeg_simd/` (~87 MB), `components/rlottie/` — decode helpers
  for the vendor UI.
- `components/sscma_client/` — the protocol client for the Himax AI co-processor
  (SPI and UART transports, 12 MHz / 921600 baud). Only relevant if the nav
  firmware ever wants the camera; it is not needed for navigation.
- `examples/factory_firmware/main/` — the shipped firmware source (~31 MB of it
  is LVGL view assets under `main/view/`). The interesting trees are
  `main/app/` (~750 KB), `main/task_flow_engine/`, `main/task_flow_module/`,
  `main/sensor/`, `main/util/`.
- `examples/openai-realtime/` (~124 MB), `examples/sscma_client_ota/` —
  voice/OTA demos with their own submodules.

## BSP API surface

`components/sensecap-watcher/include/sensecap-watcher.h` (~350 lines) is the
whole contract. Groups:

| Group | Key calls |
| --- | --- |
| Bus | `bsp_i2c_bus_init`, `bsp_spi_bus_init`, `bsp_uart_bus_init` |
| IO expander | `bsp_io_expander_init`, `bsp_exp_io_get_level`, `bsp_exp_io_set_level` |
| Display | `bsp_lvgl_init`, `bsp_lvgl_init_with_cfg`, `bsp_lvgl_get_disp`, `bsp_lcd_get_panel_handle`, `bsp_lcd_get_touch_handle`, `bsp_lcd_brightness_set` |
| Input | `bsp_knob_btn_init`, `bsp_knob_btn_get_key_value`, `bsp_set_btn_long_press_cb`, `bsp_set_btn_long_release_cb` |
| Audio | `bsp_audio_init`, `bsp_audio_codec_speaker_init`, `bsp_audio_codec_microphone_init`, `bsp_i2s_read/write`, `bsp_codec_*` |
| Power | `bsp_system_deep_sleep`, `bsp_system_reboot`, `bsp_system_shutdown`, `bsp_system_is_charging`, `bsp_system_is_standby`, `bsp_battery_is_present`, `bsp_battery_get_voltage`, `bsp_battery_get_percent` |
| RTC | `bsp_rtc_init`, `bsp_rtc_get_time`, `bsp_rtc_set_time`, `bsp_rtc_set_timer` |
| Storage | `bsp_sdcard_init[_default]`, `bsp_sdcard_is_inserted`, `bsp_spiffs_init[_default]` |
| Misc | `bsp_rgb_init`, `bsp_rgb_set`, `bsp_i2c_detect` |
| Himax | `bsp_sscma_client_init`, `bsp_sscma_flasher_init` |

Note the BSP was written for **LVGL 8 / `esp_lvgl_port` 1.4.0**:
`bsp_lvgl_init()` returns `lv_disp_t *`. LVGL 9 renamed this to
`lv_display_t` and changed the buffer API — see [porting-notes.md](porting-notes.md).

## Initialisation order the vendor uses

From `examples/get_started/main/get_started.c`:

```c
bsp_io_expander_init();                 // I2C0 + expander, raises all power rails
bsp_exp_io_set_level(BSP_PWR_AI_CHIP, 1);// Himax rail, if you need the AI chip
lvgl_disp = bsp_lvgl_init();            // SPI3 QSPI -> LCD -> touch -> LVGL task
// SD card is brought up separately, via bsp_sdcard_init_default()
```

Inside `bsp_lvgl_init()` the order is: SPI bus → LCD panel IO/init → backlight
→ touch I2C + panel → `lvgl_port_init` → `lvgl_port_add_disp` →
`lvgl_port_add_touch` (`sensecap-watcher.c:656-768`). The touch panel is read
once right after creation to settle it (`c:756-758`).

**The expander must come first.** It owns the LCD, SD and codec power rails
(`c:145-181`), so nothing downstream works until it is up.

## Flash layout

Two partition tables exist. The **factory firmware** (the one actually on a
retail device) is:

```
# Name,     Type, SubType,  Size
nvsfactory, data, nvs,       200K
nvs,        data, nvs,       840K
otadata,    data, ota,       0x2000
phy_init,   data, phy,       0x1000
ota_0,      app,  ota_0,     12M
ota_1,      app,  ota_1,     12M
model,      data, spiffs,    1024K
storage,    data, spiffs,    6080K
```

(`firmware-sdk/examples/factory_firmware/partitions.csv`. The smaller
`get_started` example offsets differ — 6M factory + 2 × 10M OTA.)

Facts that matter:

- **`nvsfactory` at 0x9000, 200 KB, holds per-unit factory data** (and this is
  where any device licence/keys live). The wiki is explicit: back it up before
  flashing, and prefer `idf.py app-flash` so it is never touched.
  `esptool.py --chip esp32s3 --baud 2000000 read_flash 0x9000 204800 nvsfactory.bin`
- The app slots are **12 MB each**, with 32 MB of flash total. A nav firmware
  has far more room here than on the Waveshare board (16 MB flash).
- SubType is `ota_0`/`ota_1` — this is a **two-slot A/B OTA** layout, no
  separate factory app slot.

## Build and flash

ESP-IDF **v5.2.1** is what the vendor pins (`wiki/.../watcher_idf_installation.md`);
the SDK's own README allows ">=5.2.0". Our repo is on IDF 5.5.2 — see
[porting-notes.md](porting-notes.md) for the mismatch.

```sh
idf.py set-target esp32s3
cd examples/get_started && idf.py build

# only the app slot — protects nvsfactory and is fast
idf.py --port /dev/ttyACM0 -b 2000000 app-flash
idf.py --port /dev/ttyACM0 monitor        # Ctrl+] to exit
```

- Use the **bottom** USB-C port; the back one is power-only.
- Two UARTs enumerate — one ESP32-S3, one Himax, order not guaranteed.
- Windows: the Flash Download Tool flow works too, with the firmware written to
  address `0x0` (see `wiki/.../_Flash_Watcher_Agent_Firmware.md`).

## Kconfig / sdkconfig highlights

From `examples/factory_firmware/sdkconfig.defaults` (IDF 5.2.1):

- `CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y`, `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y`
- `CONFIG_SPIRAM_MODE_OCT=y`, `CONFIG_SPIRAM_SPEED_80M=y`,
  `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`
- `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y`, `CONFIG_FREERTOS_HZ=1000`
- **`CONFIG_BT_ENABLED=y` + `CONFIG_BT_NIMBLE_ENABLED=y`** — the vendor firmware
  is on NimBLE too, with `ATT_PREFERRED_MTU=512`,
  `GATT_MAX_PROCS=32`, `BLE_GATT_BLOB_TRANSFER=y`,
  `MSYS_1_BLOCK_COUNT=64`, `TRANSPORT_ACL_SIZE=512`, and
  `MEM_ALLOC_MODE_EXTERNAL` (BLE buffers in PSRAM). Useful precedent for our
  own BLE tuning.
- `CONFIG_LV_COLOR_16_SWAP=y`, `CONFIG_LV_MEM_CUSTOM=y`
- `CONFIG_LVGL_PORT_TASK_PRIORITY=11`,
  `CONFIG_LVGL_PORT_TASK_AFFINITY_CPU1=y`,
  `CONFIG_LVGL_PORT_TASK_STACK_ALLOC_EXTERNAL=y`,
  `CONFIG_LVGL_INPUT_DEVICE_SENSITIVITY=20`
- `CONFIG_BSP_LCD_PANEL_SPI_TRANS_Q_DEPTH=1`,
  `CONFIG_BSP_LCD_SPI_DMA_SIZE_DIV=16`
- `CONFIG_MQTT_BUFFER_SIZE=20480` (SenseCraft cloud), `CONFIG_LV_USE_QRCODE=y`
  (device-binding QR), `CONFIG_LV_USE_PNG=y`

## The Himax link (context, probably not needed for nav)

Two transports to the HX6538:

| Transport | Pins | Rate | Use |
| --- | --- | --- | --- |
| SPI2 (`sscma_client`) | CS 21, SYNC = expander bit 6 | 12 MHz | inference in/out, images, events |
| UART1 (`sscma_flasher`) | TX 17, RX 18 | 921600 | flashing the Himax / pushing models |

Power for the Himax is a separate expander rail (`BSP_PWR_AI_CHIP`, bit 11).
If a nav firmware does not use the camera, that rail can stay off — which is
also the low-power choice.
