# SenseCAP Watcher — Documentation

Reference material for porting the **nav-esp32** companion firmware from the
Waveshare ESP32-S3-Touch-LCD-2.8C to the **Seeed Studio SenseCAP Watcher W1-A**
(<https://wiki.seeedstudio.com/watcher/>).

> **Status:** the reference material below was pulled from Seeed's wiki, their
> open-source hardware repo and their firmware SDK. The port has since been
> implemented — see [`../firmware_watcher/`](../firmware_watcher/README.md) — and
> [`porting-notes.md`](porting-notes.md) records what it took, including three
> things that were not visible in the docs.

## TL;DR — what the Watcher is

An ESP32-S3 (8 MB PSRAM, 32 MB flash) with a **1.45" round 412 × 412 QSPI
touchscreen**, a **second AI SoC** (Himax HX6538 + OV5647 camera), a wheel,
a mic and speaker, an RTC, a microSD slot and 16-bit IO expander. It runs
NimBLE and ESP-IDF 5.2.1.

The short version for us: **same SoC, almost everything else differs.** The
panel controller, touch part, expander, SD bus, RTC and wake line are all
different, and the board has no IMU. See [porting-notes.md](porting-notes.md)
for the delta table, what the port took, and the three findings that were not
visible in any of this documentation.

## Documentation map

| Doc | What it covers |
| --- | --- |
| [hardware-overview.md](hardware-overview.md) | Chips, connectors, power, serial console + shell commands |
| [pinout.md](pinout.md) | **Complete GPIO + expander map** — extracted from the vendor BSP, with file:line citations |
| [firmware-sdk.md](firmware-sdk.md) | Vendor SDK layout, BSP API, init order, partition tables, Kconfig |
| [porting-notes.md](porting-notes.md) | Watcher vs Waveshare 2.8C, and the open questions |
| [MANIFEST.md](MANIFEST.md) | Every file, with source URL and sha256 |

## Bundled material

| Path | Contents |
| --- | --- |
| `wiki/` | 35 wiki pages as raw markdown, hierarchy preserved (see below) |
| `hardware/` | Board schematic, HX6538 and ESP32-S3 datasheets (PDF) |
| `firmware-sdk/` | Curated subset of the vendor SDK — the BSP, the knob/expander components, three small examples, factory-firmware developer docs |

### `wiki/`

Pulled from the source repo that generates the wiki
([`Seeed-Studio/wiki-documents`](https://github.com/Seeed-Studio/wiki-documents),
branch `docusaurus-version`), **not** scraped from the rendered pages — the
rendered HTML is buried in navigation boilerplate, the markdown is clean and
keeps its front-matter (slug, dates, canonical URL).

Highlights:

| Page | Why it matters |
| --- | --- |
| `Firmware_architecture_Overview/watcher_architecture.md` | Task-flow engine, module model, event pipelines |
| `Firmware_architecture_Overview/watcher_idf_installation.md` | **IDF 5.2.1, `nvsfactory` backup, `app-flash`** |
| `Firmware_architecture_Overview/watcher_function_module_dev_guide.md` | How to write a task-flow module |
| `Firmware_architecture_Overview/watcher_ui_intergration_guide.md` | SquareLine Studio + LVGL UI integration |
| `Product_Overview/watcher_hardware_overview.md` | Spec table, block diagram, serial shell commands |
| `Product_Overview/watcher_operation_guideline.md` | Power on/off, buttons, settings, factory reset |
| `Software_Framework_Overview/sensecap_watcher_local_deploy.md` | Running the AI stack on-prem |
| `Notifications/watcher_uart_output.md` | The alarm packet format (binary + JSON) |
| `Applications/SenseCAP_Watcher_for_Xiaozhi_AI/` | Community firmware flashing + MCP/Grove extension |

The Chinese, Japanese, Spanish and Portuguese mirrors of these pages were
skipped — English only.

## Sources

| Source | What we took |
| --- | --- |
| <https://wiki.seeedstudio.com/watcher/> | Wiki page tree → `wiki/` |
| [`Seeed-Studio/wiki-documents`](https://github.com/Seeed-Studio/wiki-documents) | The raw markdown behind those pages |
| [`Seeed-Studio/OSHW-SenseCAP-Watcher`](https://github.com/Seeed-Studio/OSHW-SenseCAP-Watcher) | Schematic, HX6538 + ESP32-S3 datasheets, factory firmware binaries |
| [`Seeed-Studio/SenseCAP-Watcher-Firmware`](https://github.com/Seeed-Studio/SenseCAP-Watcher-Firmware) | The SDK: BSP, components, examples, developer docs |
| <https://www.seeedstudio.com/SenseCAP-Watcher-W1-A-p-5979.html> | Product page (not mirrored) |

## Licensing

The schematic, datasheets and SDK sources are Seeed / Himax / Espressif
documents **redistributed here for reference only**. The SDK is Apache-2.0 and
retains its upstream licence files (`firmware-sdk/components/*/license.txt`,
`LICENSE`). Verify redistribution terms before publishing this repository.

The factory firmware **binaries** (`Firmware/ESP32/V1.1.7/*.bin`,
`Firmware/Himax/himax_firmware_*.img`) and the 48 MB 3D shell are **not**
mirrored — grab them from the OSHW repo if you need them:

<https://github.com/Seeed-Studio/OSHW-SenseCAP-Watcher/tree/main/Firmware>
