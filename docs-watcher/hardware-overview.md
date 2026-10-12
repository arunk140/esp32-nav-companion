# SenseCAP Watcher — hardware overview

A round, camera-equipped "AI watcher" from Seeed Studio. It is an **ESP32-S3
board with a second AI SoC and a camera bolted on** — a very different product
from a plain display board.

Source: `wiki/Sensor/SenseCAP/SenseCAP_Watcher/Product_Overview/watcher_hardware_overview.md`
(rendered: <https://wiki.seeedstudio.com/watcher_hardware_overview/>), plus the
vendor BSP where noted. See [pinout.md](pinout.md) for the GPIO map.

## Specification

| | |
| --- | --- |
| **Product** | SenseCAP Watcher W1-A |
| **Main MCU** | ESP32-S3 @ 240 MHz, **8 MB PSRAM**, **32 MB flash** |
| **AI co-processor** | **Himax HX6538** (Cortex-M55 + Ethos-U55), 16 MB flash of its own |
| **Camera** | OV5647, 120° FOV, fixed focus at 3 m — wired to the Himax, not the ESP32 |
| **Display** | 1.45" round touchscreen, **412 × 412**, SPD2010 over **QSPI** |
| **Touch** | SPD2010 integrated, over I2C1 |
| **Audio** | 1 microphone (ES7243/ES7243E ADC), 1 W speaker (ES8311 DAC) |
| **Wireless** | Wi-Fi 802.11 b/g/n 2.4 GHz, Bluetooth 5 LE, internal antenna |
| **Input** | Wheel: scroll up/down + press (rotary encoder + expander button) |
| **Indicator** | 1 × addressable RGB LED |
| **Storage** | microSD up to 32 GB, FAT32 |
| **RTC** | PCF8563 on I2C0 |
| **IO expander** | 16-bit PCA9535 on I2C0 |
| **Expansion** | 1 × Grove I2C, plus a 2×4 header (I2C, 2 × GPIO, 2 × GND, 3.3 V out, 5 V in) |
| **USB** | Two USB-C: **bottom = power + programming**, back = power only |
| **Reset** | Recessed RST button in the bottom hole, plus a hardware power button |
| **Power** | 5 V DC in, 400 mAh Li-ion backup battery, 5 V/1 A recommended adaptor |
| **Operating temp** | 0 – 45 °C |

> Warned repeatedly in the docs: supply **5 V only**. A higher-voltage adaptor
> can destroy the board.

## Block diagram, as Seeed describe it

ESP32-S3 is the system master. It talks to:

- the **LCD/touch** over SPI (QSPI) + I2C,
- **audio** over I2S (mic in, speaker out),
- **flash** (32 MB) over SPI,
- the **microSD** over SPI,
- the **expansion port** over I2C,
- the **wheel** over GPIO/PWM,
- **USB** for charge and data.

Separately, the **Himax HX6538** handles the AI workloads and the **OV5647**
camera hangs off it. The two SoCs are linked over a dedicated SPI link plus a
UART used to flash the Himax.

That last point is the single biggest architectural difference from a plain
ESP32-S3 display board: **the camera and inference do not go through the
ESP32.** The ESP32 sees them through the `sscma_client` protocol on SPI2
(12 MHz) — request/response with image + inference payloads, and it can push
new model/firmware images to the Himax over UART1 at 921600.

## What ships in the box

- Watcher body with a 360° rotate bracket
- USB-C cable and power adaptor guidance
- Packaging that doubles as a calendar-style stand
- A 1/4" tripod bracket is **sold separately**

## Serial console

Plugged into a PC, the bottom USB-C port enumerates as **one USB device plus
two UARTs** — one wired to the ESP32-S3, one to the Himax. Which is which is
not fixed; try each.

The ESP32-S3 console exposes an interactive **command shell** (so the vendor
firmware is not a closed box). Useful commands, straight from the wiki:

| Command | Purpose |
| --- | --- |
| `help [<cmd>]` | list/describe commands |
| `wifi_sta -s <ssid> -p <password>` | join a 2.4 GHz AP |
| `ota -t <0\|1\|2> --url=<url>` | force OTA (0 = AI model, 1 = Himax, 2 = ESP32) |
| `taskflow -i\|-e -f <file.json>` / `-j` | import/export the task-flow JSON (via SD or stdin) |
| `factory_info` | dump factory information |
| `battery` | battery percent |
| `bsp i2cdetect <0\|1>` | scan an I2C bus — handy when writing the port |
| `reboot` / `factory_reset` | reboot / reset |
| `record -t <secs> -f <file>` | record PCM to SD |
| `vi_ctrl -s\|-e\|-c [-z <0\|1>]` | voice-interaction control (push to talk) |
| `iperf …` | throughput test |
| `rgb -r … -g … -b … -m <mode>` | drive the RGB LED (1 breath, 2 blink, 3 solid) |

## Resource links

- Product page: <https://www.seeedstudio.com/SenseCAP-Watcher-W1-A-p-5979.html>
- Videos: <https://www.youtube.com/watch?v=ny22Z0cAIqE>
- OSHW repo (schematic, datasheets, 3D shell, factory firmware):
  <https://github.com/Seeed-Studio/OSHW-SenseCAP-Watcher>
- SDK repo: <https://github.com/Seeed-Studio/SenseCAP-Watcher-Firmware>
- Wiki centre: <https://wiki.seeedstudio.com/watcher/>

Local copies of the schematic and datasheets are in [`hardware/`](hardware/).
The 3D shell (`.stp`, 48 MB) was **not** copied into the repo — fetch it from
the OSHW repo if you need it.
