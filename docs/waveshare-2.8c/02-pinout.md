# 02 — Complete Pin Map

**This is the most important document here.** The wiki only publishes the header
pin tables; the internal map below was extracted from the vendor demo sources in
`examples/`. Every row cites where it came from so it can be verified.

Paths are relative to `docs/waveshare-2.8c/`.

## The headline: the GPIO expander

The **TCA9554PWR** (I2C address `0x20`) controls five signals that have **no
direct GPIO and are not broken out anywhere**. You cannot see these in any pinout
diagram, and if you do not initialise the expander, nothing works.

| Expander pin | Function | Active | Defined in | Driven from |
| --- | --- | --- | --- | --- |
| `EXIO1` | **LCD reset** (`LCD_RST`) | low = reset | `examples/esp-idf/main/LCD_Driver/ST7701S.h:52` (`EXAMPLE_PIN_NUM_DISP_EN = -1`) | `ST7701S.c:380` |
| `EXIO2` | **Touch reset** (GT911 `RST`) | low = reset | `examples/esp-idf/main/Touch_Driver/GT911.h:65` (`I2C_Touch_RST_IO = -1`) | `GT911.c:198` |
| `EXIO3` | **LCD chip select** (SPI CS) | low = select | `examples/esp-idf/main/LCD_Driver/ST7701S.h:23` (`LCD_CS = -1`, "Using EXIO") | `ST7701S.c:389` |
| `EXIO4` | **TF card chip select** (SD `D3`) | low = select | `examples/esp-idf/main/SD_Card/SD_MMC.h:20` (`D3 = -1`, "Using EXIO") | `SD_MMC.c:12` |
| `EXIO5` | **IMU interrupt 2** (QMI8658 `INT2`) | active | Board schematic: `Extend_IO5` ↔ `IMU_INT2` | unused by demo |
| `EXIO6` | **IMU interrupt 1** (QMI8658 `INT1`) | active | Board schematic: `Extend_IO6` ↔ `IMU_INT1` | unused by demo |
| `EXIO7` | unused | — | — | — |
| `EXIO8` | **Buzzer** | high = on | `examples/arduino/TCA9554PWR.h:33` (`EXIO_PIN8`) | `Buzzer.c:5` |

> **The IMU interrupts are on the expander, not on a GPIO.** This is a design
> decision by Waveshare and it has a hard consequence: **you cannot use the
> QMI8658 data-ready interrupt.** The TCA9554PWR has no alert/IRQ line back to
> the ESP32, so an IMU interrupt event cannot reach the CPU asynchronously — and
> you certainly should not poll the expander in a tight loop to simulate it.
> **The IMU must be polled over I2C.** See
> [03-display-touch-sensors.md — Tilt and attitude fusion](03-display-touch-sensors.md#tilt-and-attitude-fusion).
>
> This is a minor constraint, not a real problem: at 50 Hz the IMU poll costs
> about **2% of the 400 kHz I2C bus**.

The `-1` values next to `DISP_EN`, `RST`, `CS` and `D3` in the vendor headers are
the tell: "not a GPIO, it lives on the expander." The wiki's statement that the
TCA9554's pins are "all used, not brought out" is exactly this.

> The Arduino header defines `EXIO_PIN1`–`EXIO_PIN8`; the ESP-IDF header defines
> `TCA9554_EXIO1`–`TCA9554_EXIO8`. Same register, different names — pick one
> convention and be consistent.

## 12-pin multifunction header

Order as published in the wiki's "12PIN Wire Interface Pin Mapping Table".

| # | Label | Function | Description |
| --- | --- | --- | --- |
| 1 | `GND` | GND | Power ground |
| 2 | `VBus` | 5 V | USB power |
| 3 | `D-` | USB differential (**GPIO19**) | USB differential or GPIO |
| 4 | `D+` | USB differential (**GPIO20**) | USB differential or GPIO |
| 5 | `GND` | GND | Power ground |
| 6 | `3V3` | 3V3 | 3.3 V output |
| 7 | `SCL` | **GPIO7** | I2C clock — **not** usable as a regular GPIO |
| 8 | `SDA` | **GPIO15** | I2C data — **not** usable as a regular GPIO |
| 9 | `TXD` | **GPIO43** | UART TX, or usable as a regular GPIO |
| 10 | `RXD` | **GPIO44** | UART RX, or usable as a regular GPIO |
| 11 | `NC` | NC | No connection |
| 12 | `IO0` | **GPIO0** | Spare GPIO |

**GPIO43/GPIO44 are the natural place to hang a GPS module.** They are free, they
are broken out, and they are not claimed by the display or any onboard device.

**GPIO0 is the strapping pin** — it is also the BOOT button. Avoid using it for
anything that must be held at a particular level at reset.

## I2C header

| Label | Function | Description |
| --- | --- | --- |
| `GND` | GND | Power ground |
| `3V3` | 3V3 | 3.3 V output |
| `SCL` | **GPIO7** | I2C clock — cannot be remapped |
| `SDA` | **GPIO15** | I2C data — cannot be remapped |

The wiki is explicit: this header is tied to the onboard I2C chips and **can only
be used to attach additional I2C devices**. It cannot be repurposed for SPI, UART,
PWM or anything else.

## UART header

| Label | Function | Description |
| --- | --- | --- |
| `GND` | GND | Power ground |
| `3V3` | 3V3 | 3.3 V output |
| `TXD` | **GPIO43** | UART TX, or regular GPIO |
| `RXD` | **GPIO44** | UART RX, or regular GPIO |

> **The UART header is disabled whenever the USB-to-UART Type-C port is
> connected.** The FSUSB42UMX switch cuts it. If you are debugging over USB while
> also using the 4-pin UART, the UART will appear dead. Unplug that Type-C, or use
> the native USB port for programming.

## Full GPIO allocation

Every GPIO on the board, and what has claimed it.

### LCD — RGB565 parallel interface

| Signal | GPIO | Notes |
| --- | --- | --- |
| `MOSI` | **1** | SPI, **init sequence only** |
| `SCLK` | **2** | SPI, **init sequence only** — shared with SD CLK |
| `PCLK` | **41** | Pixel clock |
| `HSYNC` | **38** | |
| `VSYNC` | **39** | |
| `DE` | **40** | Data enable |
| `DISP` | — | Routed to `EXIO1` |
| IMU `INT1` | — | Routed to `EXIO6` |
| IMU `INT2` | — | Routed to `EXIO5` |

RGB565 data lines (`examples/esp-idf/main/LCD_Driver/ST7701S.h:36-51`,
identical in `examples/arduino/Display_ST7701.h:61-76`):

| Bit | Colour | GPIO | | Bit | Colour | GPIO |
| --- | --- | --- | --- | --- | --- | --- |
| `DATA0` | B0 | **5** | | `DATA8` | G3 | **11** |
| `DATA1` | B1 | **45** | | `DATA9` | G4 | **10** |
| `DATA2` | B2 | **48** | | `DATA10` | G5 | **9** |
| `DATA3` | B3 | **47** | | `DATA11` | R0 | **46** |
| `DATA4` | B4 | **21** | | `DATA12` | R1 | **3** |
| `DATA5` | G0 | **14** | | `DATA13` | R2 | **8** |
| `DATA6` | G1 | **13** | | `DATA14` | R3 | **18** |
| `DATA7` | G2 | **12** | | `DATA15` | R4 | **17** |

| Signal | GPIO | Notes |
| --- | --- | --- |
| Backlight | **6** | PWM via LEDC channel 0 |

### I2C bus — GPIO7 / GPIO15, 400 kHz, `I2C_NUM_0`

| Device | Address | Notes |
| --- | --- | --- |
| GT911 touch | **`0x5D`** | `examples/arduino/Touch_GT911.h:7` |
| QMI8658 IMU | **`0x6B`** | `QMI8658_Init()` selects `QMI8658_L_SLAVE_ADDRESS`, which is `0x6B` (`QMI8658.h:6`, `QMI8658.c:25`). `0x6A` is defined as `..._H_...` but is **not used** — the naming is counter-intuitive. |
| PCF85063 RTC | **`0x51`** | `examples/esp-idf/main/PCF85063/PCF85063.h:7` |
| TCA9554 expander | **`0x20`** | `examples/esp-idf/main/EXIO/TCA9554PWR.h` |

Bus config: `I2C_MASTER_NUM = 0`, `I2C_MASTER_FREQ_HZ = 400000`,
`I2C_MASTER_TIMEOUT_MS = 1000` (`examples/esp-idf/main/I2C_Driver/I2C_Driver.h`).

> Four devices on one 400 kHz bus. The vendor driver polls the IMU in a 100 ms
> loop, so contention is not a practical problem today. If you add a fast sensor
> or a second bus device, remember there is no second I2C peripheral brought out —
> you would have to bit-bang or share.

### Touch

| Signal | GPIO | Notes |
| --- | --- | --- |
| `INT` | **16** | Real GPIO. Falling-edge interrupt (`GT911.c:61`) |
| `RST` | — | Routed to `EXIO2` |

**GPIO16 is the only interrupt line on the board that is a plain GPIO.** It is what
makes touch-driven wake-from-sleep possible — worth using for a battery-powered
nav device.

### TF / microSD card

| Signal | GPIO | Notes |
| --- | --- | --- |
| `CLK` | **2** | **Shared with LCD `SCLK`** |
| `CMD` | **1** | **Shared with LCD `MOSI`** |
| `D0` | **42** | |
| `D1` | — | Not connected (1-bit mode) |
| `D2` | — | Not connected |
| `D3` / CS | — | Routed to `EXIO4` |

`examples/esp-idf/main/SD_Card/SD_MMC.h:15-20`; Arduino equivalent
`examples/arduino/SD_Card.h:9-11`.

> **GPIO1 and GPIO2 are shared between the LCD's SPI port and the SD card's
> controller.** The two peripherals fight over the same two pins. The vendor
> survives this by convention, not by hardware isolation:
>
> - The Arduino sketch comments say it explicitly — *"It must be initialized after
>   the LCD, and if the LCD is reinitialized later, the SD also needs to be
>   reinitialized"* (`examples/arduino/LVGL_Arduino.ino:37-38`).
> - Both the Arduino and IDF examples call `SD_Init()` **after** `LCD_Init()`.
>
> **Never reinitialise the LCD without reinitialising the SD card afterwards.**
> This is the single most fragile interaction on the board.

### Battery / ADC

| Signal | GPIO | Notes |
| --- | --- | --- |
| `VBAT` sense | **4** | ADC1 channel 3, `ADC_ATTEN_DB_12` (`examples/esp-idf/main/BAT_Driver/BAT_Driver.h:13-14`) |

Uses the legacy `adc_oneshot` API. Calibration is attempted; if it fails the code
degrades to an uncalibrated reading rather than aborting. A `Measurement_offset`
of `0.980952` is applied to compensate the divider.

### Free / available GPIO

⚠️ **Corrected after checking the ESP32-S3 datasheet and the board schematic.**
An earlier version of this document claimed GPIO26–32 and GPIO35–37 were free.
**They are not.** This board has very little GPIO headroom.

| GPIO | Status | Notes |
| --- | --- | --- |
| **43, 44** | ✅ **Free** | **The only clean, free, broken-out GPIOs.** On both the 12-pin and 4-pin UART headers. Nothing onboard claims them. |
| **0** | ⚠️ Avoid | BOOT button + strapping pin. Also broken out as `IO0` on the 12-pin header, but the button shorts it to ground. |
| **19, 20** | ⚠️ Only if you give up USB | Native USB D−/D+. Usable as plain GPIO only if you never want USB again. |
| **26–32** | ❌ **Not usable** | **Zero occurrences in the board schematic** — not routed to anything, so physically unreachable. Datasheet also marks them P4 "not recommended" (reserved for the off-package flash/PSRAM bus). |
| **33–37** | ❌ **Consumed by PSRAM** | The in-package **octal** PSRAM on the ESP32-S3R8 uses these as the high 4 data bits + DQS. See below. |
| **45, 46** | ❌ **Do not use** | Strapping pins. GPIO45 selects the flash voltage (3.3 V vs 1.8 V); GPIO46 is a boot-mode select. Using either risks a bad boot. |
| **3** | ❌ Used | LCD `DATA12` — *and* a strapping pin. It works only because the LCD does not drive during boot. |

#### Why GPIO33–37 are not free

The ESP32-S3R8 has **8 MB of in-package Octal SPI PSRAM** (ESP32-S3 datasheet
v2.2, Table 1-1). Datasheet section 2.3.4 is explicit:

> "GPIO33, GPIO34, GPIO35, GPIO36, GPIO37: The higher 4 bits data line interface
> and DQS interface for the SPI0/1 interface in 8-line SPI mode, **and can be GPIO
> pins if the chip is not connected to flash or PSRAM in 8-line SPI mode**."

This chip *is* connected in 8-line mode. The datasheet also carries a blanket
warning: *"Do not use the pins connected to in-package flash/PSRAM for any other
purposes."*

#### Why GPIO26–32 are not free either

Datasheet section 2.3.4, Priority 4 (P4) — "already allocated or **not
recommended for use**":

> "GPIO26, GPIO27, GPIO28, GPIO29, GPIO30, GPIO31, GPIO32: SPI0/1 interface
> connected to the in-package flash and PSRAM, **or recommended for the
> off-package flash and PSRAM**."

The board's **16 MB flash is off-package** (a discrete chip on the PCB, since
the bare ESP32-S3R8 has no in-package flash). That bus needs the recommended
pins. Independently, the board schematic contains **no net labels for
IO26–IO32 at all**, so they are not routed to a header and cannot be reached
without soldering directly to the module.

#### Net result

**Two usable GPIOs: GPIO43 and GPIO44.** Both broken out. That is the entire
expansion budget.

> **This is the single most important hardware constraint for an add-on design.**
> It rules out I2S audio (3 pins) and any multi-pin peripheral unless the pin can
> be shared with the existing I2C bus. Plan audio and sensors around **I2C**, which
> needs no new GPIO — GPIO7/GPIO15 are already on a header and only 4 devices use
> the bus. See [09-hardware-notes-for-nav.md](09-hardware-notes-for-nav.md).
>
> Note the expander has just **one** unused pin (`EXIO7`), so a shift register on
> it is not a viable expansion route.

## Peripheral bus summary

| Peripheral | Shared with | Safe to reconfigure? |
| --- | --- | --- |
| SPI (ST7701S init) | SD card CLK/CMD | **No** — order-dependent |
| SPI (TF card) | LCD init SPI | **No** — order-dependent |
| I2C0 (400 kHz) | Touch, IMU, RTC, expander, I2C header | No — one bus |
| LEDC ch 0 | Backlight (GPIO6) | Yes, if you accept a fixed backlight |
| ADC1 ch 3 | Battery sense (GPIO4) | Yes |

## Conflicts to design around

1. **LCD ⇄ TF card** share GPIO1/GPIO2. Re-init ordering is mandatory.
2. **Expander must initialise before LCD, touch, or SD.** Without it, SPI chip
   select floats and the panel never wakes.
3. **UART header ⇄ USB-to-UART Type-C** are mutually exclusive by design.
4. **GPIO0 is the BOOT/strapping pin** — not a general-purpose output.
5. **I2C header cannot be remapped** to any other function.
