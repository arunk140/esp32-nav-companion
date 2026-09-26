# 08 — Resources and Links

Everything the wiki links, with notes on which are actually useful for a
navigation build.

## Bundled locally

| File | What |
| --- | --- |
| [`datasheets/ESP32-S3-Touch-LCD-2.8C_schematic_diagram.pdf`](datasheets/) | **Board schematic — start here for anything not documented** |
| [`datasheets/ST7701S_SPEC_V1.4.pdf`](datasheets/) | LCD driver IC |
| [`datasheets/GT911_EN_Datasheet.pdf`](datasheets/) | Touch controller |
| [`datasheets/QMI8658C.pdf`](datasheets/) | IMU |
| [`datasheets/PCF85063A.pdf`](datasheets/) | RTC |
| [`datasheets/TCA9554.pdf`](datasheets/) | GPIO expander |
| [`datasheets/esp32-s3_datasheet_en.pdf`](datasheets/) | SoC |
| [`reference/ESP32-S3-Touch-LCD-2.8C-structure.zip`](reference/) | Mechanical drawing (PDF + DWG) |
| [`examples/arduino/`](examples/arduino/) | Arduino sketch + 12 driver files |
| [`examples/esp-idf/`](examples/esp-idf/) | ESP-IDF project (main tree + build config) |

Checksums and source URLs: [MANIFEST.md](MANIFEST.md).

## Examples

| Item | Link |
| --- | --- |
| **ESP32-S3-Touch-LCD-2.8C demo archive** (71 MB — Arduino + ESP-IDF + test firmware) | [Download](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8C/ESP32-S3-Touch-LCD-2.8C-Demo.zip) |
| Board support requests | <https://github.com/waveshareteam/ESP32-display-support/tree/master> |
| Waveshare org | <https://github.com/waveshareteam> |

The archive layout:

```
Arduino/
  examples/LVGL_Arduino/     ← the Arduino sketch (mirrored in examples/arduino/)
  libraries/lvgl/            ← LVGL 8.3.10 (~70 MB; not committed here)
ESP-IDF/
  ESP32-S3-Touch-LCD-2.8C-Test/
    main/                    ← (mirrored in examples/esp-idf/main/)
    components/lvgl__lvgl/   ← LVGL for IDF
Firmware/
  ESP32-S3-Touch-LCD-2.8C.bin/   ← test firmware, flash at 0x00
```

Vendor `ReadMe.txt` also warns:

> "If the compilation succeeds the first time and the compilation fails during
> subsequent tests, decompress the folder again and compile the new file."

## Hardware

| Item | Link |
| --- | --- |
| Product page (buy) | <https://www.waveshare.com/esp32-s3-touch-lcd-2.8c.htm?sku=29086> |
| 3D drawing | `ESP32-S3-Touch-LCD-2.8C-20241226.zip` from the [wiki Resources page](https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8C/Resources-And-Documents) |
| Project drawing | `ESP32-S3-Touch-LCD-2.8C-structure.zip` — bundled |
| 12-pin / 4-pin ribbon cables | 1.0 mm pitch, 100 mm (shown on the product page) |

## Datasheets

| Part | Link |
| --- | --- |
| ESP32-S3 datasheet | <https://documentation.espressif.com/esp32-s3_datasheet_en.pdf> |
| ESP32-S3 technical reference manual | <https://documentation.espressif.com/esp32-s3_technical_reference_manual_en.pdf> |
| QMI8658 | <https://files.waveshare.com/wiki/common/QMI8658C.pdf> |
| ST7701S | <https://files.waveshare.com/wiki/common/ST7701S_SPEC_V1.4.pdf> |
| GT911 | <https://files.waveshare.com/wiki/common/GT911_EN_Datasheet.pdf> |
| PCF85063A | <https://files.waveshare.com/wiki/common/PCF85063A.pdf> |
| TCA9554 | <https://www.ti.com/lit/ds/symlink/tca9554.pdf> |

## Software tools

| Tool | Link |
| --- | --- |
| Flash Download Tool (Windows) | <https://dl.espressif.com/public/flash_download_tool.zip> |
| ESP-IDF Installation Manager | <https://dl.espressif.com/dl/eim/> |
| ESP-IDF get-started | <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html> |
| ESP-IDF extension for VS Code | <https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension> |
| ESP32 Arduino core docs | <https://docs.espressif.com/projects/arduino-esp32/en/latest/index.html> |
| **LVGL documentation** | <https://docs.lvgl.io/master/> |
| LVGL on Arduino | <https://docs.lvgl.io/master/get-started/platforms/arduino.html> |
| SquareLine Studio tutorial | <https://www.waveshare.com/wiki/Waveshare_SquareLine_Studio> |
| Esptool | ships with ESP-IDF (`python -m esptool`) |

## Waveshare tutorials

> Both series use the **ESP32-S3-Zero** as the teaching board. The wiki says so
> and warns to check your own board's pinout. Do not copy pin numbers from them.

**Arduino** (14 sections): <https://docs.waveshare.com/ESP32-Arduino-Tutorials>

Getting to know ESP32 · Arduino IDE setup · Arduino basics · Digital I/O ·
Analog input · PWM · UART · **I2C** · SPI · Wi-Fi · Web server · Bluetooth ·
**LVGL GUI** · Comprehensive project

**ESP-IDF** (9 sections): <https://docs.waveshare.com/ESP32-ESP-IDF-Tutorials>

Environment setup · Running examples · Creating a project · Using components ·
Debugging · **FreeRTOS** · **Peripherals** · Wi-Fi · BLE

Also: [ESP32 Tutorials index](https://docs.waveshare.com/ESP32-Tutorials-Intro)

## Community showcase

From the wiki's Resources page. Independently maintained by their creators.

### Instrument-cluster and dashboard projects — most relevant to nav UI

| Project | Links |
| --- | --- |
| **traviscea** — DIY Round LCD Digital Dash | [Video](https://www.youtube.com/watch?v=t7H6pevep40) · [GitHub](https://github.com/traviscea?tab=repositories) |
| **traviscea** — Coding a DIY Digital Dash Front-End (ESP32 + LVGL) | [Video](https://www.youtube.com/watch?v=ff-u4jB2Mug) |
| **traviscea** — DIY Round Digital Dash Part 3: the backend/brain | [Video](https://www.youtube.com/watch?v=5Tu0i3o-3ps) |
| **Boating with the Baileys** — custom marine displays with ESP32 & SignalK | [Video](https://www.youtube.com/watch?v=d4MlVSbn_wI) · [GitHub](https://github.com/Boatingwiththebaileys/Marine-Displays/tree/main) |
| **DriftAndDeploy** — Digital Gauge for the ND (Miata) | [Reddit](https://www.reddit.com/r/MiataND/comments/1w2mwh6/digital_gauge_for_the_nd/) · [GitHub](https://github.com/DriftAndDeploy/MiataDigitalGauge) |

> The `traviscea` series is a **round** display digital dash on ESP32 with LVGL —
> the closest published analogue to this project. Worth studying for round-display
> layout technique and for the gauge-drawing approach. The SignalK marine project
> covers NMEA-style data ingestion and dynamic icons.

### Other

| Project | Links |
| --- | --- |
| Using an ESP32-S3 display to learn English | [Video](https://www.youtube.com/watch?v=QjIxRxdhQ3E) |

## Related hardware worth considering

Listed on the product page, and relevant if this board's constraints turn out to
bite:

| Board | Why it might be better |
| --- | --- |
| [ESP32-S3-Touch-LCD-2.1](https://www.waveshare.com/esp32-s3-touch-lcd-2.1.htm) | Same 480 × 480 round format, **262K colour**, smaller |
| [ESP32-S3-Touch-LCD-1.85C](https://www.waveshare.com/esp32-s3-touch-lcd-1.85c.htm) | 360 × 360 round, aluminium case, **AI speech** |
| [ESP32-S3-Touch-LCD-1.85B](https://www.waveshare.com/esp32-s3-touch-lcd-1.85b.htm) | 360 × 360, CNC case, LLM platform support |
| [ESP32-S3-Touch-LCD-1.8](https://www.waveshare.com/esp32-s3-knob-touch-lcd-1.8.htm) | 1.8" knob display, CNC case |
| [ESP32-S3-Touch-LCD-4.3C](https://www.waveshare.com/esp32-s3-touch-lcd-4.3c.htm) | 800 × 480, **5-point touch, dual-mic array** |
| [ESP32-S3-Touch-LCD-7B](https://www.waveshare.com/esp32-s3-lcd-7b.htm) | 1024 × 600 — if a map needs real estate |
| [ESP32-S3-LCD-3.16](https://www.waveshare.com/esp32-s3-lcd-3.16.htm) | 320 × 820, **262K colour** |
| [GNSS / GPS modules](https://www.waveshare.com/product/iot-communication/long-range-wireless/gnss-gps.htm) | The missing piece for this project |

> The 2.1" board keeps the 480 × 480 round format but is **262K colour** rather
> than 65K. If RGB565 banding proves unacceptable for map rendering, that is a
> meaningful difference — at the cost of a slightly smaller screen.

## Support

| Channel | Link |
| --- | --- |
| Technical support page | <https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8C/Technical-Support> |
| General support | <https://docs.waveshare.com/support> · <https://service.waveshare.com/> |
| Bulk customisation / sales | <https://service.waveshare.com/> |
| Forum | <https://www.waveshare.com/forum/> |

**Reminder from the FAQ:** Waveshare does not provide firmware customisation
support. The engineers will answer technical questions, but you own all the code.
