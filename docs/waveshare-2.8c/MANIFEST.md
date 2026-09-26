# MANIFEST

Every file fetched into `docs/waveshare-2.8c/`, with its source URL, size and
SHA-256 (first 16 hex chars) as retrieved on **2026-09-25**.

Re-fetch with:

```bash
cd docs/waveshare-2.8c
find . -type f -not -name "*.md" | sort | \
  xargs -I{} sh -c 'printf "%s  %8s  %s\n" "$(sha256sum "{}" | cut -c1-16)" "$(stat -c%s "{}")" "{}"'
```

## Datasheets

All from Waveshare/Espressif/TI.

| File | Size | SHA-256 (16) | Source |
| --- | ---: | --- | --- |
| `datasheets/ESP32-S3-Touch-LCD-2.8C_schematic_diagram.pdf` | 946,158 | `01eae811f2b777b3` | <https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8C/ESP32-S3-Touch-LCD-2.8C_schematic_diagram.pdf> |
| `datasheets/esp32-s3_datasheet_en.pdf` | 1,098,115 | `2d5a7cb7fd559d8d` | <https://documentation.espressif.com/esp32-s3_datasheet_en.pdf> |
| `datasheets/ST7701S_SPEC_V1.4.pdf` | 8,502,996 | `ef682eda3c4f2264` | <https://files.waveshare.com/wiki/common/ST7701S_SPEC_V1.4.pdf> |
| `datasheets/GT911_EN_Datasheet.pdf` | 1,634,301 | `227d240eb4344a64` | <https://files.waveshare.com/wiki/common/GT911_EN_Datasheet.pdf> |
| `datasheets/QMI8658C.pdf` | 2,851,772 | `5518afa3ec3a5c3b` | <https://files.waveshare.com/wiki/common/QMI8658C.pdf> |
| `datasheets/PCF85063A.pdf` | 726,288 | `a09223dbb6427808` | <https://files.waveshare.com/wiki/common/PCF85063A.pdf> |
| `datasheets/TCA9554.pdf` | 2,043,261 | `efce888fff720618` | <https://www.ti.com/lit/ds/symlink/tca9554.pdf> |

The **ESP32-S3 technical reference manual** was linked by the wiki but not
downloaded — it is ~1,500 pages and mostly irrelevant without a bare-metal
project: <https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf>

## Reference / mechanical

| File | Size | SHA-256 (16) | Source |
| --- | ---: | --- | --- |
| `reference/ESP32-S3-Touch-LCD-2.8C-structure.zip` | 1,633,332 | `9ff2068171a3691a` | <https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8C/ESP32-S3-Touch-LCD-2.8C-structure.zip> |

Contains `ESP32-S3-Touch-LCD-2.8C-20241226.pdf` and `.dwg` (the display module
mechanical drawing). Dimensions were read from it and from
`media/10-board-dimensions.jpg`; see
[01-hardware-overview.md](01-hardware-overview.md#mechanical).

The **3D drawing** (`ESP32-S3-Touch-LCD-2.8C-20241226.zip`) was not downloaded —
it is a large CAD archive and unnecessary for firmware work:
<https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8C/ESP32-S3-Touch-LCD-2.8C-20241226.zip>

## Example sources

Extracted from
[`ESP32-S3-Touch-LCD-2.8C-Demo.zip`](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8C/ESP32-S3-Touch-LCD-2.8C-Demo.zip)
(70,960,575 bytes, SHA-256 begins `f93eff03664f7dde`). Extracted paths,
unmodified.

### `examples/arduino/` — from `Arduino/examples/LVGL_Arduino/`

| File | Size | SHA-256 (16) |
| --- | ---: | --- |
| `LVGL_Arduino.ino` | 1,554 | `da9b48678658f2c1` |
| `Display_ST7701.cpp` | 14,733 | `b86f9ac09fa54983` |
| `Display_ST7701.h` | 4,354 | `72e017c9c15ea13a` |
| `LVGL_Example.cpp` | 24,496 | `e9d9f6036f49cf10` |
| `LVGL_Example.h` | 377 | `e72b63c0829387ca` |
| `LVGL_Driver.cpp` | 3,836 | `fca913639a070983` |
| `LVGL_Driver.h` | 856 | `c0b7e100777dcdc0` |
| `Gyro_QMI8658.cpp` | 9,013 | `5b9c704ede28cb21` |
| `Gyro_QMI8658.h` | 5,273 | `5b9c704ede28cb21` |
| `Touch_GT911.cpp` | 5,298 | `3e63ec1e116ffb83` |
| `Touch_GT911.h` | 1,237 | `6ee7098ce225ae56` |
| `RTC_PCF85063.cpp` | 7,443 | `c7ff03ad937cac0c` |
| `RTC_PCF85063.h` | 3,060 | `7262287fc2e669b2` |
| `TCA9554PWR.cpp` | 4,935 | `ec790005562912c2` |
| `TCA9554PWR.h` | 3,678 | `2fa95775f43a4539` |
| `SD_Card.cpp` | 4,219 | `b467ddb5ca3b85b1` |
| `SD_Card.h` | 480 | `355f9ef9cd3fd989` |
| `BAT_Driver.cpp` | 411 | `ef706a4f2edced58` |
| `BAT_Driver.h` | 189 | `921b133953a9fdd1` |
| `I2C_Driver.cpp` | 960 | `4e75f3f59d2f7540` |
| `I2C_Driver.h` | 308 | `635738fb16654a2d` |
| `Wireless.cpp` | 1,906 | `25070e07f8f08067` |
| `Wireless.h` | 478 | `21cd10157748d5a9` |
| `lvgl-library.properties` | 391 | `6c306df544ed9245` |

> `lvgl-library.properties` was **renamed** from `library.properties` (copied out
> of the LVGL tree) to record the LVGL version — 8.3.10. It is the only file here
> not at its original path.

### `examples/esp-idf/` — from `ESP-IDF/ESP32-S3-Touch-LCD-2.8C-Test/`

| File | Size | SHA-256 (16) |
| --- | ---: | --- |
| `CMakeLists.txt` | 130 | `f90edcb0c02b3628` |
| `partitions.csv` | 279 | `4760303a643b82a7` |
| `sdkconfig.defaults` | 365 | `43b1dcb691cf0845` |
| `sdkconfig.defaults.esp32s3` | 295 | `2cc6074056519ece` |
| `main/main.c` | 1,592 | `39399d485a1e2972` |
| `main/CMakeLists.txt` | 1,547 | `b401acc26c4e2261` |
| `main/idf_component.yml` | 51 | `d79f97801a1f2846` |
| `main/Kconfig.projbuild` | 2,186 | `da0c82acee56e0da` |
| `main/BAT_Driver/BAT_Driver.c` | 4,715 | `34e575f9f685639d` |
| `main/BAT_Driver/BAT_Driver.h` | 605 | `6bc35b07d9758ceb` |
| `main/Buzzer/Buzzer.c` | 149 | `a231adb0aeebd387` |
| `main/Buzzer/Buzzer.h` | 90 | `67e15beb6dfefcae` |
| `main/EXIO/TCA9554PWR.c` | 5,575 | `a92844e3590f4f2a` |
| `main/EXIO/TCA9554PWR.h` | 4,077 | `d6d4f31c02d6163b` |
| `main/I2C_Driver/I2C_Driver.c` | 2,053 | `d309b0daa631629d` |
| `main/I2C_Driver/I2C_Driver.h` | 1,086 | `908e4f523145e793` |
| `main/LCD_Driver/ST7701S.c` | 15,901 | `9e850ec0f65ea54f` |
| `main/LCD_Driver/ST7701S.h` | 3,747 | `8d9da53ca5127151` |
| `main/LVGL_Driver/LVGL_Driver.c` | 4,116 | `dd8c99192492fcbe` |
| `main/LVGL_Driver/LVGL_Driver.h` | 676 | `e81392949418f1cc` |
| `main/LVGL_UI/LVGL_Example.c` | 15,939 | `6e87d7e50cdbec0e` |
| `main/LVGL_UI/LVGL_Example.h` | 458 | `6f1a48b04991d243` |
| `main/PCF85063/PCF85063.c` | 6,896 | `c8df49554f78e6fa` |
| `main/PCF85063/PCF85063.h` | 2,959 | `83f878ee6b86d289` |
| `main/QMI8658/QMI8658.c` | 8,725 | `82918dbcbd164a2c` |
| `main/QMI8658/QMI8658.h` | 5,113 | `3ee12fcda504d1dd` |
| `main/SD_Card/SD_MMC.c` | 4,539 | `a8f19439fec99fdb` |
| `main/SD_Card/SD_MMC.h` | 759 | `a2543113d3a8e496` |
| `main/Touch_Driver/GT911.c` | 8,848 | `b3895cabe383641c` |
| `main/Touch_Driver/GT911.h` | 2,030 | `80c99005ef2c7e67` |
| `main/Touch_Driver/esp_lcd_touch/esp_lcd_touch.c` | 6,910 | `2727b60babfb3c3a` |
| `main/Touch_Driver/esp_lcd_touch/esp_lcd_touch.h` | 11,009 | `326401d3b06d87e5` |
| `main/Wireless/Wireless.c` | 7,589 | `9814efa4911f4b1f` |
| `main/Wireless/Wireless.h` | 520 | `3b57b1ae7017e6b1` |

> `examples/esp-idf/README.md` came from the archive but is the upstream project's
> own file; it was not checksummed here. It contains two **dangling links** to
> `main/rgb_lcd_example_main.c`, a filename from the upstream Espressif
> `rgb_lcd_panel` example rather than this project. Left untouched — it is a
> verbatim vendor artefact.

### Deliberately not committed

| Item | Size | Reason |
| --- | ---: | --- |
| `Arduino/libraries/lvgl/` | ~70 MB | Vendored LVGL 8.3.10. Fetch from the component manager or the demo archive. Version recorded in `examples/arduino/lvgl-library.properties`. |
| `ESP-IDF/.../components/lvgl__lvgl/` | ~1,900 files | Same, for the IDF build. Resolved via `main/idf_component.yml`. |
| `Firmware/ESP32-S3-Touch-LCD-2.8C.bin` | 16,777,216 | Prebuilt test firmware, flash at `0x00`. A binary is not documentation. Download on demand — see [06-flashing.md](06-flashing.md). |
| `pytest_rgb_panel_lvgl.py` | — | Not yet copied. Available in the archive; it is the regression harness for RGB panel + LVGL changes. |
| `.vscode/`, `.devcontainer/` | — | Upstream VS Code and container config. Not needed yet. |

## Media

From the wiki (`docs.waveshare.com/assets/images/...`) and the product page
(`www.waveshare.com/img/devkit/...`).

| File | Size | SHA-256 (16) | What |
| --- | ---: | --- | --- |
| `media/01-product-front.webp` | 23,654 | `8fb5e323ba2314b5` | Board, front |
| `media/02-onboard-resources.webp` | 88,294 | `39d32d1b77988feb` | Onboard resources callout |
| `media/03-dimensions.webp` | 69,228 | `b5637eb0b0f58ac5` | Wiki dimension image |
| `media/04-wiring-tfcard.webp` | 80,456 | `7a5da6dd215dbefc` | TF card insertion + USB connection |
| `media/05-demo-ui-a.webp` | 431,550 | `90b525dad1349d56` | Demo UI |
| `media/06-demo-ui-b.webp` | 268,896 | `94706c3939cf61ce` | Demo UI |
| `media/07-flash-tool-config.webp` | 16,452 | `85b74e84215c41e1` | Flash tool settings |
| `media/08-flash-file-setup.webp` | 23,176 | `27e4b1527d227f6d` | File + address `0x00` |
| `media/09-flash-done.webp` | 28,830 | `606a7c4bb394ce33` | Flash complete |
| `media/10-board-dimensions.jpg` | 288,527 | `e0c1681f0df2afa1` | **Dimension drawing — authoritative** |
| `media/11-product-detail-1.jpg` | 158,448 | `f23e2cb5f3362d5d` | Product detail |
| `media/12-product-detail-2a.jpg` | 9,301 | `38e16975f6e3236b` | Product detail |
| `media/13-product-detail-2b.jpg` | 10,228 | `ebc572315921d95b` | Product detail |
| `media/14-product-detail-3.jpg` | 132,672 | `aeb2a4617f914ede` | Product detail |
| `media/15-product-detail-5.jpg` | 102,346 | `aa75a77862327646` | Product detail |
| `media/16-product-detail-7.jpg` | 252,611 | `93ff49d174fad1fa` | Product detail |
| `media/17-product-detail-9.jpg` | 91,312 | `05397ba912e87024` | Product detail |
| `media/18-product-intro.jpg` | 338,263 | `5d0f55fe38abb5a9` | Product intro |
| `media/19-product-alt-2.jpg` | 41,249 | `72369ff3ce4984ef` | Product alt view |

> `media/10-board-dimensions.jpg` is the source of the 95.86 mm / 70.13 mm /
> 66.00 mm / 65.34 mm / ΦM2 figures. Where it disagrees with the wiki, **it
> wins** — the wiki has no numbers, only an image.

## Wiki pages consulted

All six pages of the board's wiki section, fetched 2026-09-25:

| Page | URL |
| --- | --- |
| Overview | <https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8C> |
| Resources | <https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8C/Resources-And-Documents> |
| Working with Arduino | <https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8C/Arduino> |
| Working with ESP-IDF | <https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8C/ESP-IDF> |
| Firmware Flashing and Erasing | <https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8C/Firmware-Flashing> |
| FAQ | <https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8C/FAQ> |

> **The pin tables on the overview page are inside collapsed `<details>`
> elements** and do not appear in a plain text or markdown fetch. They were
> recovered from the raw HTML. A Chinese mirror exists at
> `docs.waveshare.net` under the same paths if the English site changes.

The Technical Support page
(<https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8C/Technical-Support>) was not
mirrored; it contains contact information only.

## Corrections made during extraction

Recorded so the next person does not re-introduce them:

- **QMI8658 I2C address is `0x6B`, not `0x6A`.** The header defines
  `QMI8658_L_SLAVE_ADDRESS (0x6B)` and `QMI8658_H_SLAVE_ADDRESS (0x6A)`, and
  `QMI8658_Init()` assigns `..._L_`. The `L`/`H` naming does not correspond to
  the numeric value. An early draft of `02-pinout.md` and
  `03-display-touch-sensors.md` recorded `0x6A`; both are corrected.
- **Board dimensions are not on the wiki.** The wiki has an image but no numbers.
  `95.86 mm` OD, `70.13 mm` active view, `66.00 mm` board width, `65.34 mm`
  height and `4 × ΦM2` come from `media/10-board-dimensions.jpg`, the product
  page, and the CAD drawing.
- **Display DMA bandwidth** was initially miscalculated as ~99 MB/s. Correct
  figures are **~60 MB/s at 107.8 Hz** and **~36 MB/s at 64.7 Hz**.
- **GPIO26–32 and GPIO33–37 are NOT free.** An early draft of `02-pinout.md`
  listed them as available expansion pins. Both claims are wrong:
  - GPIO33–37 are the high data bits + DQS of the **in-package octal PSRAM**
    (ESP32-S3R8). ESP32-S3 datasheet v2.2 §2.3.4 permits them as GPIO only if
    the chip is *not* connected in 8-line SPI mode — and it is.
  - GPIO26–32 appear **zero times** in the board schematic (not routed, so
    unreachable) and are P4 "not recommended" in the datasheet, reserved for the
    off-package flash bus.
  - GPIO35/36/37 are also **not** strapping pins; only GPIO0, GPIO3, GPIO45 and
    GPIO46 are.
  - The real expansion budget is **GPIO43 and GPIO44 only**. See
    `02-pinout.md` and the "Audio" section of `09-hardware-notes-for-nav.md`.
- **The IMU interrupts are not on GPIOs.** `IMU_INT1` and `IMU_INT2` route to the
  **TCA9554PWR expander** (`Extend_IO6` → `INT1`, `Extend_IO5` → `INT2`), found by
  tracing net labels in the board schematic. `02-pinout.md` originally listed
  `EXIO5`/`EXIO6` as unused; they are not. The expander has **one** spare pin
  (`EXIO7`), not three.
  - Consequence: the QMI8658 data-ready interrupt **cannot** be used, because the
    expander has no alert line to the SoC. The IMU must be polled — which costs
    ~1.9% of the I2C bus at 50 Hz. See `03-display-touch-sensors.md`.
- **The QMI8658 vendor defaults suit a bench, not a vehicle.** `QMI8658.c:7-10`
  sets both ODRs to 8000 Hz (160× oversampled against a 100 ms poll) and the gyro
  range to ±64 °/s, which saturates on anything but gentle motion. See
  `03-display-touch-sensors.md` for recommended overrides.

## Provenance of the pin map

[02-pinout.md](02-pinout.md) is the one document that is **not** a wiki summary.
Every GPIO in it was read out of the vendor demo sources bundled here, and each
row cites its file and line. The `-1` sentinel values in the vendor headers
(`LCD_CS -1`, `EXAMPLE_PIN_NUM_DISP_EN -1`, `I2C_Touch_RST_IO -1`,
`CONFIG_EXAMPLE_PIN_D3 -1`) are what revealed the TCA9554 expander's role — the
wiki mentions the expander exists but never says what it controls.

## Redistribution

These are Waveshare, Espressif, TI and NXP-derived documents and community source
code, mirrored for personal reference. The demo sources retain their original
SPDX headers. **Verify redistribution terms before publishing this repository
publicly.**
