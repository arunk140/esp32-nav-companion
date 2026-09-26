# 03 — Display, Touch, Sensors, Storage

## Display — ST7701S, RGB565

### Interface

The panel is driven by an **RGB565 parallel interface**, not SPI. A real SPI
peripheral (GPIO1/GPIO2) exists but carries only the ST7701S initialisation
sequence; pixel data goes out the 16-bit parallel bus.

`examples/esp-idf/main/LCD_Driver/ST7701S.h:15-16` selects the method:

```c
#define SPI_METHOD 1
#define IOEXPANDER_METHOD 0
```

`ST7701S_newObject(..., SPI2_HOST, SPI_METHOD)` — the LCD init SPI runs on
**SPI2**. `LCD_CS` is `-1` because chip select is on the expander (`EXIO3`).

### Resolution and colour depth

| Property | Value |
| --- | --- |
| Horizontal resolution | 480 |
| Vertical resolution | 480 |
| Data width | 16 bits (RGB565) |
| Panel capability | 262K (RGB888) |
| **Achievable** | **65K (RGB565)** |

The vendor FAQ is direct about this:

> "The screen supports 262K colors, but due to ESP limitations, RGB565 is used,
> resulting in an actual 65K colors."

This is an ESP32 RGB-peripheral constraint, not a wiring one. Plan your palette
for 16-bit colour and you will avoid banding surprises in gradients and dark
sky/road rendering — both common in map UIs.

### Timings

Both the Arduino and ESP-IDF examples use **identical porches**:

| Parameter | Value | Source |
| --- | --- | --- |
| H sync pulse width | 8 | `examples/arduino/Display_ST7701.h:37`, `examples/esp-idf/main/LCD_Driver/ST7701S.c` |
| H back porch | 10 | |
| H front porch | 50 | |
| V sync pulse width | 2 | |
| V back porch | 18 | |
| V front porch | 8 | |
| PCLK active-low | No (rising edge) | |

Derived totals: **H = 8 + 10 + 480 + 50 = 548**, **V = 2 + 18 + 480 + 8 = 508**
(548 × 508 = 278,384 pixel clocks per frame).

**The two examples disagree on pixel clock:**

| Example | `pclk_hz` | Resulting refresh |
| --- | --- | --- |
| ESP-IDF (`ST7701S.h:28`) | 18 MHz | **~64.7 Hz** |
| Arduino (`Display_ST7701.h:35`) | 30 MHz | **~107.8 Hz** |

The Arduino figure is closer to what you want for a scrolling map. The IDF
example runs the panel slower. If you find motion in the IDF build looking soft or
laggy, raising `EXAMPLE_LCD_PIXEL_CLOCK_HZ` toward 30 MHz is the first knob to
try — but verify the panel tolerates it, and watch for DMA bandwidth pressure
(see below).

### Frame buffer and PSRAM

```c
.data_width   = 16,
.num_fbs      = EXAMPLE_LCD_NUM_FB,   // 1 or 2, see below
.psram_trans_align = 64,
.flags.fb_in_psram = true,
.clk_src      = LCD_CLK_SRC_DEFAULT,
```

| Setting | IDF | Arduino |
| --- | --- | --- |
| Frame buffers | 1 or 2 via `CONFIG_EXAMPLE_DOUBLE_FB` (`ST7701S.h:54-58`) | 1 (`FRAME_BUF_NUM = 1`) |
| Bounce buffer | `CONFIG_EXAMPLE_USE_BOUNCE_BUFFER` | `10 × 480` px, always on |
| Tear avoidance | `CONFIG_EXAMPLE_AVOID_TEAR_EFFECT_WITH_SEM` | via bounce buffer |

The Kconfig makes the trade-off explicit — **the three options are mutually
exclusive** (`main/Kconfig.projbuild`):

```kconfig
config EXAMPLE_DOUBLE_FB
config EXAMPLE_USE_BOUNCE_BUFFER
    depends on !EXAMPLE_DOUBLE_FB
config EXAMPLE_AVOID_TEAR_EFFECT_WITH_SEM
    depends on !EXAMPLE_DOUBLE_FB
```

So you choose exactly one strategy: two full frame buffers, a bounce buffer, or
double-buffering with vsync semaphores.

A single 480 × 480 RGB565 buffer is **460,800 bytes (450 KiB)**. Two buffers is
~0.9 MB. All of it comes out of the 8 MB PSRAM.

**Double buffering costs 450 KiB but eliminates tearing.** The IDF example
supports tear-effect avoidance with semaphores
(`CONFIG_EXAMPLE_AVOID_TEAR_EFFECT_WITH_SEM`) that block the GUI until the panel
scan-out completes. For a map renderer that scrolls continuously, this is the
difference between a usable and an unusable display.

The Arduino example adds a **bounce buffer** (10 × 480 px) instead, with the
comment *"to avoid screen drift."* This is a third strategy: a small FIFO
absorbs DMA jitter without the memory cost of a full second frame buffer. Worth
benchmarking both approaches.

### PSRAM performance settings

`sdkconfig.defaults.esp32s3`:

```
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
```

The vendor comment explains the last two:

> "Enabling the following configurations can help increase the PCLK frequency in
> the case when the Frame Buffer is allocated from the PSRAM and fetched by EDMA"

And `sdkconfig.defaults` sets `CONFIG_ESP32S3_DATA_CACHE_LINE_64B` /
`..._SIZE=64` — a **DMA alignment requirement**, not a tuning knob.

> **The 800 mA LDO and the PSRAM DMA bandwidth are the two real performance
> ceilings on this board.** Display traffic = refresh rate × 278,384 px × 2 bytes:
> **~60 MB/s at 107.8 Hz**, or **~36 MB/s at 64.7 Hz**. If you also want Wi-Fi, a
> GPS on UART, and map decompression, you are competing for the same bus. Measure
> before assuming a higher refresh is achievable.

### Backlight

| Property | Value |
| --- | --- |
| GPIO | **6** |
| ESP-IDF | LEDC timer 0, channel 0, 13-bit resolution |
| Arduino | Channel 1, 20 kHz, 10-bit |
| `ON` level | 1 |
| `OFF` level | `!ON` = 0 |

The two examples use **different LEDC channels** (0 vs 1). The ESP-IDF example
has a `LEDC_TEST_DUTY` of 4000 and `Backlight_MAX 100`.

> A 2.8" backlight at full brightness is one of the largest loads on the board.
> For a battery-powered nav device, PWM-dimming the backlight to match ambient
> light is the single biggest power win available. The 20 kHz Arduino frequency
> is above audible range — keep it there, or above ~20 kHz, or the buzzer and
> PWM will produce an audible whine.

## Touch — GT911

| Property | Value |
| --- | --- |
| Controller | GT911, capacitive |
| Interface | I2C, address **`0x5D`** |
| Clock | 400 kHz, on GPIO7 |
| `INT` | **GPIO16**, falling-edge interrupt |
| `RST` | Via expander `EXIO2` |
| Max points | `CONFIG_ESP_LCD_TOUCH_MAX_POINTS` (`GT911.c:124`). Not set in this project's `Kconfig.projbuild`, so it takes the `esp_lcd_touch` component default. |

Source: `examples/esp-idf/main/Touch_Driver/GT911.h:64-65` (pin defines),
`GT911.c:190-204` (reset sequence), `GT911.c:253-256` (touch config).

### Reset sequence

The GT911 requires a specific power-on reset dance
(`touch_gt911_reset`, `GT911.c:190-204`), and it is more elaborate than a simple
pulse:

1. Drive `INT` (GPIO16) **low** as an output
2. Wait 150 ms
3. `EXIO2` low, wait 150 ms — hardware reset asserted
4. `EXIO2` high, wait 50 ms — hardware reset released
5. Drive `INT` **high**, then switch GPIO16 to **input**

The INT line is used as a reset strap during power-up, which is why it must be
reconfigured to an input afterwards. If you reimplement touch init, do not skip
the GPIO16 direction change — the driver will appear to work on a cold boot and
fail intermittently on a warm one.

### Coordinate system

The touch config at `GT911.c:253-256` is:

```c
esp_lcd_touch_config_t tp_cfg = {
    .x_max = EXAMPLE_LCD_V_RES,     // 480
    .y_max = EXAMPLE_LCD_H_RES,     // 480
    .rst_gpio_num = I2C_Touch_RST_IO,   // -1 → EXIO2
    .int_gpio_num = I2C_Touch_INT_IO,   // 16
    .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
```

Note the **axes are crossed** — `x_max` is fed the *vertical* resolution. With a
square 480 × 480 panel this is invisible, but it is a vendor quirk worth knowing
if you ever reuse this driver on a non-square display.

The demo also reads the GT911 configuration register at init
(`touch_gt911_read_cfg`, `GT911.c:209`) and prints the interrupt trigger mode:

```c
printf("TouchPad_INT:%d\r\n", (buf[3]&0x03));
```

Values `0x01` = rising edge, `0x02` = falling edge. The demo registers a
**falling-edge** interrupt regardless (`GT911.c:61`), which is worth checking
against your own config.

> **Orientation must be handled explicitly.** A round display that can be mounted
> in any of four orientations needs the touch axes rotated and possibly swapped
> to match the rendered image. The QMI8658 IMU is the natural orientation
> reference — see below.

## IMU — QMI8658

6-axis: 3-axis gyroscope + 3-axis accelerometer.

| Property | Value |
| --- | --- |
| Address | **`0x6B`** | `QMI8658_L_SLAVE_ADDRESS`. The name is counter-intuitive: `..._L_` is `0x6B` and `..._H_` is `0x6A`. `QMI8658_Init()` uses `..._L_`, so the device is at **`0x6B`**. |
| Interface | I2C, 400 kHz, GPIO7/GPIO15 |
| Interface mode | **I2C only** (register `CTRL1` bit 0 selects SPI vs I2C) |
| Interrupt | **Not usable** — `INT1`/`INT2` are routed to the TCA9554 expander (`EXIO6`/`EXIO5`), not to an ESP32 GPIO. **Must be polled.** |

Source: `examples/esp-idf/main/QMI8658/QMI8658.h:6-7`, `QMI8658.c:25`. The
driver's own doc comment says the address is *"typically 0x6A or 0x6B"* — the
breakout is tied to the address-selection pin, so if you ever get an I2C NAK here,
try the other one.

### Vendor defaults are wrong for a vehicle

`QMI8658.c:7-10` sets:

```c
acc_scale_t  acc_scale  = ACC_RANGE_4G;        // ±4 g
gyro_scale_t gyro_scale = GYR_RANGE_64DPS;     // ±64 °/s
acc_odr_t    acc_odr    = acc_odr_norm_8000;  // 8000 Hz
gyro_odr_t   gyro_odr   = gyro_odr_norm_8000; // 8000 Hz
```

Both output data rates are set to **8 kHz**, but `QMI8658_Loop()` is called from a
100 ms task — a **160× oversampling** that produces data nobody reads, while
burning power and widening the internal LPF bandwidth. `setAccLPF()`/`setGyroLPF()`
exist if you want the filtering.

For a nav device, override these:

| Setting | Vendor | Suggested | Why |
| --- | --- | --- | --- |
| `acc_odr` | 8000 Hz | **100–200 Hz** | Still 2–4× your read rate; far less power and noise |
| `gyro_odr` | 8000 Hz | **100–200 Hz** | As above |
| `acc_scale` | ±4 g | ±4 g is fine | Normal braking is 0.5–0.8 g; emergency ~1 g |
| `gyro_scale` | ±64 °/s | **±256 or ±512 °/s** | ±64 clips on anything but gentle motion. Tilt comes from the accelerometer, so lost gyro resolution costs little. |

The gyro range matters more than it looks: ±64 °/s saturates on a motorcycle, a
handheld device, or a hard corner — and a saturated gyro breaks the fusion below.

### Tilt and attitude fusion

**Tilt (pitch and roll) is an accelerometer job, not a gyroscope job.** The
accelerometer measures the gravity vector directly, giving an absolute angle with
no drift. The gyroscope measures *rate of change*, which is smooth but integrates
error. You want both, fused with a **complementary filter**:

```c
/* Accel → absolute angle (noisy, contaminated by linear acceleration) */
float accel_pitch = atan2f(-ax, sqrtf(ay*ay + az*az));
float accel_roll  = atan2f(       ay, sqrtf(ax*ax + az*az));

/* Gyro → rate (smooth, drifts) */
pitch += gyro_x * dt;          /* keep radians or degrees consistent */
roll  += gyro_y * dt;

/* Fuse. alpha ~0.95-0.98 at 50 Hz → ~0.3-0.6 s time constant */
pitch = alpha * pitch + (1.0f - alpha) * accel_pitch;
roll  = alpha * roll  + (1.0f - alpha) * accel_roll;
```

Roughly 30 floating-point operations per axis. On a 240 MHz dual-core LX7 with an
FPU this is **nowhere near measurable** — no PSRAM, negligible CPU.

**Axis mapping:** the QMI8658's axes almost certainly do not line up with the
display's; the chip sits on the PCB at whatever orientation the layout demanded.
Determine the mapping empirically — hold the device flat, note which axis reads
+1 g — then bake a rotation into the calculation. The vendor driver does **no** axis
remapping; `getAccelerometer()` just reports raw `Accel.x/y/z`
(`QMI8658.c:263-275`).

#### The accel-gating problem in a vehicle

This is what makes a naive implementation look broken. In a car, linear
acceleration **biases the apparent gravity vector**:

- Road vibration — high frequency, mostly handled by the LPF
- **Braking and acceleration — low frequency, directly corrupts pitch/roll**
- Cornering — biases roll

The standard fix is **magnitude gating**: if the measured vector length is
meaningfully different from 1 g, the reading is contaminated, so trust the gyro
alone for that sample.

```c
float mag = sqrtf(ax*ax + ay*ay + az*az);
float err = fabsf(mag - 1.0f) / 1.0f;      /* 0 = at rest, 1 = 2 g */
float trust = (err > 0.15f) ? 0.0f : 1.0f;  /* 0 = gyro only */

pitch = alpha * pitch + (1.0f - alpha) * accel_pitch * trust;
roll  = alpha * roll  + (1.0f - alpha) * accel_roll  * trust;
```

Without this the tilt display lurches every time the driver brakes. With it, tilt
holds steady through a hard stop and only the gyro carries it briefly.

**Calibrate the zero-g offset at boot** while the device is still: average a few
hundred samples, subtract the bias, keep it. The chip has an on-device path
(`CTRL9` command `0x12`, `CAL1`–`CAL4` registers 0x0B–0x12, used in
`QMI8658.c:250-254`), but doing it in software is simpler to reason about.

#### Why the missing interrupt costs nothing

Because `INT1`/`INT2` are stranded on the expander, polling is mandatory anyway —
and it is cheap. A 12-byte read (6 accel + 6 gyro) at 400 kHz is ~150 bits of bus
time:

| Rate | I2C utilisation | |
| --- | ---: | --- |
| 10 Hz (vendor default) | 0.4% | too slow to look smooth |
| 50 Hz | **1.9%** | **recommended** |
| 100 Hz | 3.8% | if you want extra smoothness |

The bus carries only 4 other devices, all polled at 10 Hz. Ample room.

#### Onboard AttitudeEngine — an alternative

`CTRL6` (0x07) configures the QST **AttitudeEngine**, a sensor-fusion engine inside
the chip. The vendor driver explicitly skips it — `QMI8658.c:221` writes `0x00`,
and the header comment says *"AttitudeEngine settings (we don't use these)"*.

It would offload fusion to hardware and output fused Euler angles directly,
eliminating the polling-and-fusing task. Worth knowing it exists, but software
fusion is more flexible, easier to debug, and is what lets you do the accel gating
a car needs. Prefer software unless power becomes critical.

The schematic shows `IMU_INT1` and `IMU_INT2` nets from the QMI8658. **These are
not routed to an ESP32 GPIO** — they go to the TCA9554PWR expander
(`Extend_IO6` → `INT1`, `Extend_IO5` → `INT2`). The expander has no alert line back
to the SoC, so an IMU interrupt cannot reach the CPU asynchronously. The driver
polls instead:

```c
while (((QMI8658_receive(QMI8658_STATUSINT)) & 0x80) == 0x00);   // QMI8658.c:98
```

This is a **busy-wait spin loop** inside a 100 ms task. For a navigation device this
matters:

- **Good news:** the accelerometer gives you an orientation reference, tilt for
  display, and a shake-to-wake gesture. See
  [Tilt and attitude fusion](#tilt-and-attitude-fusion) for the algorithm, the
  sensor ranges to override, and the acceleration gating a car needs.
- **Cost note:** 100 ms is far too slow for anything that should look smooth. Move
  the IMU onto its own ~20 ms (50 Hz) task — the bus has ample room.

Registers of note: `CTRL1` (0x02) interface + sensor enable, `CTRL2` (0x03)
accelerometer, `CTRL3` (0x04) gyro, `CTRL5` (0x06) low-pass filter, `STATUSINT`
(0x2D) status, `TEMP_L/H` (0x33/0x34) die temperature, calibration registers
`CAL1`–`CAL4` (0x0B–0x12).

## RTC — PCF85063

| Property | Value |
| --- | --- |
| Address | **`0x51`** |
| Interface | I2C, 400 kHz |
| Year offset | **1970** (`YEAR_OFFSET`) |
| Backup battery | Yes — dedicated header, rechargeable cell |
| Alarm | Supported (`RTC_CTRL_2_AIE`, alarm registers 0x0B–0x0E) |

Source: `examples/esp-idf/main/PCF85063/PCF85063.h`.

> **The RTC loses time when power is removed without a backup cell.** The vendor
> wiki says this plainly: *"The RTC time may not match the current time because
> data cannot be retained when power is off. To keep the RTC time accurate, you
> need to connect an RTC battery and update the RTC time."*
>
> **For a navigation device this is a decision point, not an afterthought.**
> A GPS gives you UTC once it has a fix, so a dead RTC is recoverable — but only
> after a cold-start fix, which is 30–60 s outdoors. If the device is expected to
> show a correct clock immediately at power-on, **fit the RTC backup battery** and
> treat the GPS as the slow sync source. The offset of 1970 (not 2000) means the
> RTC stores a Unix epoch directly, which is convenient for NMEA timestamps.

## TF / microSD card

| Signal | GPIO |
| --- | --- |
| CLK | 2 (**shared with LCD SCLK**) |
| CMD | 1 (**shared with LCD MOSI**) |
| D0 | 42 |
| D1, D2 | Not connected |
| D3 / CS | `EXIO4` |

1-bit SD/MMC mode. `examples/esp-idf/main/SD_Card/SD_MMC.h:15-20`.

### Init order is mandatory

From `examples/arduino/LVGL_Arduino.ino:37-38`:

```cpp
LCD_Init();   // If you later reinitialize the LCD, you must initialize the SD card again !!!!!!!!!!
SD_Init();    // It must be initialized after the LCD, and if the LCD is reinitialized later,
              // the SD also needs to be reinitialized
```

The IDF example follows the same order in `main.c:49-51`:

```c
LCD_Init();
Touch_Init();
SD_Init();
```

The SPI peripheral is configured by the LCD first. Anything that re-initialises
SPI2 (LCD re-init, a display sleep/wake cycle, a backlight-off power save) will
break the card until `SD_Init()` runs again. **This is the highest-risk
interaction on the board for a firmware that sleeps the display.**

Full boot order from `examples/esp-idf/main/main.c:45-54`:

```
Wireless_Init()
Driver_Init()   → Flash_Searching, BAT_Init, I2C_Init, PCF85063_Init,
                  QMI8658_Init, EXIO_Init, then spawns Driver_Loop task
LCD_Init()
Touch_Init()
SD_Init()
LVGL_Init()
Lvgl_Example1()
```

And `Driver_Loop()` runs as a FreeRTOS task **pinned to core 0**, priority 3,
4096-byte stack, looping every 100 ms over IMU, RTC and battery voltage
(`main.c:17-27`, task created at `main.c:36-42`).

### Card requirements

From the vendor FAQ: if the card is not recognised, **format it FAT32**. If the
first attempt fails, wait a moment and reset before concluding it is faulty.

For map data, remember the card is 1-bit — sequential throughput is adequate for
map tiles, but it will not keep up with a sustained high-rate video stream.

## Battery

| Property | Value |
| --- | --- |
| Sense pin | **GPIO4**, ADC1 channel 3 |
| Attenuation | 12 dB (`ADC_ATTEN_DB_12`) |
| Calibration | Attempted via `adc_cali_*`; degrades gracefully if unavailable |
| Divider correction | `Measurement_offset = 0.980952` |
| API | Legacy `adc_oneshot` (ADC2 not used) |

Source: `examples/esp-idf/main/BAT_Driver/BAT_Driver.h:13-16`, `BAT_Driver.c:80-112`.

ADC1 is used, which is fortunate — ADC1 works alongside Wi-Fi, whereas ADC2 does
not. Keep it that way.

## Wireless

`Wireless_Init()` is called first in both examples, before all hardware init. The
demo performs a Wi-Fi scan and displays the count.

> **Antenna:** there is an IPEX1 connector for an external antenna, but it requires
> **re-soldering an onboard resistor** to switch over. That is not a jumper or a
> software setting — it is a physical modification. Waveshare does not document
> which resistor. For a navigation device, GPS and Wi-Fi will sit adjacent and
> interfere; an external antenna may be worth the rework, but confirm the
> resistor location from the schematic in
> [`datasheets/ESP32-S3-Touch-LCD-2.8C_schematic_diagram.pdf`](datasheets/).
