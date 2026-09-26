# nav-esp32 — Documentation

Reference material for building a navigation device on the
**Waveshare ESP32-S3-Touch-LCD-2.8C**.

> **Status:** documentation only. No firmware has been written yet.
> The next phase is firmware design — see [Hardware notes for a nav device](waveshare-2.8c/09-hardware-notes-for-nav.md)
> for the constraints that will shape that design.

## Contents

| Document | What it covers |
| --- | --- |
| [waveshare-2.8c/README.md](waveshare-2.8c/README.md) | Board index, TL;DR spec, doc map |
| [01-hardware-overview.md](waveshare-2.8c/01-hardware-overview.md) | Chips, connectors, buttons, power |
| [02-pinout.md](waveshare-2.8c/02-pinout.md) | **Complete GPIO map** — the one you will actually use |
| [03-display-touch-sensors.md](waveshare-2.8c/03-display-touch-sensors.md) | ST7701S RGB panel, GT911 touch, QMI8658 IMU, PCF85063 RTC |
| [04-development-arduino.md](waveshare-2.8c/04-development-arduino.md) | Arduino IDE setup and toolchain pinning |
| [05-development-esp-idf.md](waveshare-2.8c/05-development-esp-idf.md) | ESP-IDF setup, sdkconfig, partitions |
| [06-flashing.md](waveshare-2.8c/06-flashing.md) | Flashing and erasing the test firmware |
| [07-faq-troubleshooting.md](waveshare-2.8c/07-faq-troubleshooting.md) | FAQ + gotchas discovered while extracting the pin map |
| [08-resources-links.md](waveshare-2.8c/08-resources-links.md) | Datasheets, tools, community projects |
| [09-hardware-notes-for-nav.md](waveshare-2.8c/09-hardware-notes-for-nav.md) | Implications for a GPS/navigation build |
| [10-map-tiles-reference.md](waveshare-2.8c/10-map-tiles-reference.md) | `0015/map_tiles` assessment — SD-card maps, LVGL 9 conflict, known bugs |
| [MANIFEST.md](waveshare-2.8c/MANIFEST.md) | Every downloaded file, with source URL and checksum |

## Bundled assets

| Path | Contents |
| --- | --- |
| `waveshare-2.8c/examples/arduino/` | Arduino example — 12 driver files |
| `waveshare-2.8c/examples/esp-idf/` | ESP-IDF example — full `main/` tree + build config |
| `waveshare-2.8c/datasheets/` | 7 datasheets incl. board schematic |
| `waveshare-2.8c/reference/` | Mechanical structure files (STEP) |
| `waveshare-2.8c/media/` | Product photos, dimension drawing, demo UI |

## Source

All content derived from the official Waveshare wiki:
<https://docs.waveshare.com/ESP32-S3-Touch-LCD-2.8C>

The pin map in `02-pinout.md` was **not** published on the wiki — it was extracted
from the vendor demo sources in `examples/`. Each row cites the file and line it
came from, so every pin can be traced to a definition.
