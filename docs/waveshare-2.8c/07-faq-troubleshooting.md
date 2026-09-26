# 07 — FAQ and Troubleshooting

## Vendor FAQ

Verbatim from the board's FAQ page.

### Q: RGB screen issue

> "The screen supports 262K colors, but due to ESP limitations, RGB565 is used,
> resulting in an actual 65K colors."

**Practical effect:** design for 16-bit colour. Gradients across few colours will
band. If you need smoother shading — a sky gradient over a map, a speedometer
arc — use dithering, or pre-render assets with a limited palette rather than
assuming 24-bit.

### Q: How to get more library support for the example?

> "You can subscribe to this repository and raise an issue to describe your
> requirements. The engineers will assess your request as soon as possible:
> [ESP32-display-support](https://github.com/waveshareteam/ESP32-display-support/tree/master)"

That repository is the right place to look for board support beyond what ships in
the demo archive.

### Q: Why does the flashing fail?

> "1. When the serial port is occupied, the flashing will fail. Close the serial
> port monitor and try to flash again.
>
> 2. When the ESP32 program crashes, the flashing will fail. In this case, you need
> to completely power off the development module, hold down BOOT button and power
> it on again to enter the forced download mode and then flash it. After flashing,
> the module will not automatically exit download mode, so you need to power cycle
> again."

Covered in detail in [06-flashing.md](06-flashing.md).

### Q: How to check which COM port I am using?

See [06-flashing.md — Finding the COM port](06-flashing.md#finding-the-com-port).

### Q: Can you help me review or modify my code? Can you help me modify the code?

> "This product is positioned as a development board, not a finished product...
> We do not assist in modifying code. Please let the makers and geeks use their
> DIY skills."

**This is the most important FAQ entry for planning.** Waveshare will not help
with firmware. All software work is ours. For bulk hardware customisation they
point to [sales](https://service.waveshare.com/).

### Q: How to use SquareLine Studio to design interfaces?

> "Please refer to [SquareLine Studio Tutorial](https://www.waveshare.com/wiki/Waveshare_SquareLine_Studio)."

Remember to generate **LVGL v8** output to match 8.3.10.

---

## Gotchas discovered while extracting the pin map

These are **not** in the vendor FAQ. They came out of reading the driver sources
and are the things most likely to cost you a weekend.

### 1. Five signals are hidden behind the GPIO expander

The TCA9554PWR (I2C `0x20`) drives **LCD reset, LCD chip-select, touch reset, TF
card chip-select and the buzzer**. None of these have a GPIO and none are broken
out. The wiki's phrase "all pins used, not brought out" refers to exactly this.

The tell in the vendor headers is a value of `-1` next to a pin define:

```c
#define LCD_CS  -1      // Using EXIO      <- ST7701S.h:23
#define EXAMPLE_PIN_NUM_DISP_EN  -1      <- ST7701S.h:52
#define CONFIG_EXAMPLE_PIN_D3  -1  // Using EXIO   <- SD_MMC.h:20
```

**Symptom if you miss it:** SPI chip-select floats, the panel never initialises,
screen stays blank, and the logs show no error. `EXIO_Init()` must run after
`I2C_Init()` and before `LCD_Init()`.

**Symptom if you forget `EXIO2` (touch reset):** touch appears dead while the
display works fine. The GT911 power-on reset requires the expander pulse.

### 2. GPIO1 and GPIO2 are shared between the LCD and the TF card

The LCD's init SPI and the SD controller both claim GPIO1 (MOSI/CMD) and
GPIO2 (SCLK/CLK). There is no hardware arbitration — the vendor relies on
initialisation order.

The Arduino sketch states it in caps:

> `LCD_Init();` — *"If you later reinitialize the LCD, you must initialize the SD
> card again !!!!!!!!!!"*

> `SD_Init();` — *"It must be initialized after the LCD, and if the LCD is
> reinitialized later, the SD also needs to be reinitialized"*

**Symptom:** SD card stops working after you add a display sleep/wake or backlight
power-save cycle. **Fix:** re-run `SD_Init()` after any SPI2 reconfiguration.

This is the highest-risk interaction on the board, and it is invisible until you
add a power-management feature.

### 3. The 4-pin UART header is disabled by the USB-to-UART Type-C port

The FSUSB42UMX switch cuts the header whenever that Type-C is plugged in.

**Symptom:** a GPS module on GPIO43/44 works during development, then goes dead
when you plug in USB to flash. **Fix:** unplug the USB-to-UART cable, or program
over the native USB port instead.

### 4. Two different pixel clocks between the two examples

18 MHz (ESP-IDF, ~64.7 Hz) vs 30 MHz (Arduino, ~107.8 Hz). Same porches. If the
IDF build looks sluggish, this is why — and raising it is a legitimate fix, not a
hack.

### 5. LVGL 8 vs 9 is a hard break, and the two examples pin different 8.x versions

Arduino pins **8.3.10**; the IDF manifest requests **`~8.2.0`**. SquareLine
Studio will cheerfully generate v9 code. Mixing any of these will not compile, or
will compile and misbehave.

### 6. The IMU driver busy-waits, and the IMU interrupts are unused

`QMI8658.c:98` spins on the status register inside a 100 ms task:

```c
while (((QMI8658_receive(QMI8658_STATUSINT)) & 0x80) == 0x00);
```

The schematic shows `IMU_INT1`/`IMU_INT2` from the QMI8658, but the driver never
uses them. That spin loop costs real CPU. For a nav device, consider enabling IMU
data-ready interrupts — after confirming the lines are routed on your board
revision.

### 7. Touch INT (GPIO16) is repurposed as a reset strap at power-on

The GT911 init drives GPIO16 **low as an output**, waits 150 ms, pulses `EXIO2`,
waits 50 ms, then drives GPIO16 **high and switches it to input**. If you skip the
direction change, touch will work on a cold boot and fail intermittently on a warm
boot. This is a genuinely nasty class of bug.

### 8. The 3 MB app partition is small

`partitions.csv` allocates 3 MB to `factory`. LVGL plus fonts plus a map renderer
plus a GNSS parser can exceed that. Check `idf.py size` early.

### 9. `CONFIG_ESP32S3_DATA_CACHE_LINE_*` is a DMA requirement, not a tuning knob

```
CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y
CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE=64
```

Changing these for "performance" will produce corrupted or absent display output.

### 10. GPIO26–32 are probably not usable — verify before relying on them

The vendor code never touches GPIO26–32, which hints they are free. But with
16 MB of flash on an ESP32-S3 module, those pins are commonly used by the on-module
SPI flash. **Run a GPIO toggle test and scope it before designing around them.**
The genuinely safe expansion pins are **GPIO43/44** (broken out, used by nothing
onboard) and **GPIO35/36/37** (no onboard function, though they are strapping
pins).

### 11. The RTC forgets time without a backup cell

> "The RTC time may not match the current time because data cannot be retained when
> power is off. To keep the RTC time accurate, you need to connect an RTC battery
> and update the RTC time."

For a nav device, fit the battery if you want a correct clock at power-on.
Otherwise a cold GPS start (30–60 s) is your only sync. The RTC uses a **1970**
year offset, so its register value is a Unix epoch directly.

### 12. The vendor's own examples have build-order fragility

From the demo archive's `ReadMe.txt`:

> "If the compilation succeeds the first time and the compilation fails during
> subsequent tests, decompress the folder again and compile the new file."

If you hit a nonsensical build failure with the stock example, re-extract rather
than debugging.

### 13. The external antenna needs a soldering iron

The IPEX1 connector requires **re-soldering an onboard resistor** to switch to an
external antenna. Waveshare does not document which resistor. Check
[`datasheets/ESP32-S3-Touch-LCD-2.8C_schematic_diagram.pdf`](datasheets/).

### 14. The board is a 95.86 mm circle, and your UI corners will be invisible

The framebuffer is 480 × 480 but the glass is a 70.13 mm circle. The canvas corners
fall outside the visible area — roughly 21.5% of the canvas. LVGL will happily
render there and the pixels will simply never be seen.

**Fix:** clip to a circle, mask with a background, or design a layout that uses
the round shape deliberately. Details in
[01-hardware-overview.md](01-hardware-overview.md#what-a-9586-mm-round-display-means-for-a-nav-ui).

### Symptom → cause quick reference

| Symptom | Most likely cause |
| --- | --- |
| Blank screen, no error in log | `EXIO_Init()` missing or misordered |
| Display works, touch dead | `EXIO2` reset pulse missing |
| SD works at boot, dies later | LCD re-init without `SD_Init()` |
| Dead SD after plugging in USB | UART header cut by the FSUSB42UMX switch |
| Compile error in LVGL code | LVGL 8 vs 9 mismatch |
| Scrolling looks sluggish on IDF build | 18 MHz pixel clock |
| Text vanishes at screen edges | Canvas corners outside the round glass |
| Won't flash, "waiting for sync" | Not in download mode — full power cycle with BOOT held |
| Flashing fails randomly | Serial monitor holding the port |
| Build fails for no reason on stock example | Re-extract the archive |
| Display works, then corrupts after a sleep/wake | SPI2 reconfigured — re-init SD |
