# SenseCAP Watcher — GPIO map

**Status:** complete for the ESP32-S3 side, extracted from the vendor BSP.
Like the Waveshare pin map, this was **not** published as a table by Seeed — it
was read out of the board support package that ships in the vendor SDK.

Source (vendored copy, unmodified):
`firmware-sdk/components/sensecap-watcher/include/sensecap-watcher.h`
Line numbers below refer to that file. The initialisation code that consumes
these defines is in
`firmware-sdk/components/sensecap-watcher/sensecap-watcher.c`.

Upstream: <https://github.com/Seeed-Studio/SenseCAP-Watcher-Firmware/blob/main/components/sensecap-watcher/include/sensecap-watcher.h>

## At a glance

- The board has **two I2C buses and two SPI buses**, all on fixed pins.
- The **LCD is QSPI, not parallel RGB** — 4 data lines + clock, plus a CS.
- **Everything power-related, plus the knob button and both card/touch
  interrupts, sits behind a 16-bit PCA95xx I2C expander.** Its `INT` line is
  the only GPIO that can wake the chip from deep sleep.
- **GPIO 1 is shared**: it is QSPI `DATA1` for the LCD *and* the BSP's nominal
  "LCD DC" pin. In QSPI mode `dc_gpio_num` is set to `-1`, so only the QSPI role
  is live (see `sensecap-watcher.c:613`).
- SPI2 is **shared between the Himax AI module and the SD card**.

## ESP32-S3 GPIO

### SPI2 — shared: Himax AI module + microSD

| Signal | GPIO | Source |
| --- | --- | --- |
| SCLK | 4 | `BSP_SPI2_HOST_SCLK` (h:44) |
| MOSI | 5 | `BSP_SPI2_HOST_MOSI` (h:45) |
| MISO | 6 | `BSP_SPI2_HOST_MISO` (h:46) |
| Himax CS | 21 | `BSP_SSCMA_CLIENT_SPI_CS` (h:73) |
| microSD CS | 46 | `BSP_SD_SPI_CS` (h:85) |

Himax bus runs at 12 MHz (`h:76`). SD shares the bus on a different CS (`h:84`).

### SPI3 / QSPI — LCD panel (SPD2010)

| Signal | GPIO | Source |
| --- | --- | --- |
| PCLK | 7 | `BSP_SPI3_HOST_PCLK` (h:49) |
| DATA0 | 9 | `BSP_SPI3_HOST_DATA0` (h:50) |
| DATA1 | 1 | `BSP_SPI3_HOST_DATA1` (h:51) |
| DATA2 | 14 | `BSP_SPI3_HOST_DATA2` (h:52) |
| DATA3 | 13 | `BSP_SPI3_HOST_DATA3` (h:53) |
| CS | 45 | `BSP_LCD_SPI_CS` (h:90) |
| DC | — (NC) | `BSP_LCD_GPIO_DC` is defined as GPIO1 (h:92) but QSPI mode passes `dc_gpio_num = -1` (`c:613`) |
| RST | — (NC) | `BSP_LCD_GPIO_RST` = `GPIO_NUM_NC` (h:91); reset is shared with touch |
| Backlight (PWM) | 8 | `BSP_LCD_GPIO_BL` (h:93), LEDC channel 1, timer 1, 5 kHz, 10-bit (`c:569-578`) |

Panel parameters (`h:131-147`): 412 × 412, 40 MHz pixel clock, 32-bit command /
8-bit parameter framing, RGB565, `swap_xy`/`mirror_x`/`mirror_y` all 0,
quad mode enabled (`c:620`), QSPI interface (`c:625`).

### I2C1 — touch

| Signal | GPIO | Source |
| --- | --- | --- |
| SDA | 39 | `BSP_TOUCH_I2C_SDA` (h:98) |
| SCL | 38 | `BSP_TOUCH_I2C_SCL` (h:99) |
| Interrupt | expander P0.5 | `BSP_TOUCH_GPIO_INT` (h:97) |

400 kHz (`h:100`). Touch controller is the **SPD2010** (integrated with the
display part) driven by `esp_lcd_touch_spd2010` (`c:752-754`). Reset and
interrupt pins are declared `GPIO_NUM_NC` in the touch config (`c:740-741`) —
the interrupt is read through the expander instead.

### I2C0 — "general" bus

| Signal | GPIO | Source |
| --- | --- | --- |
| SDA | 47 | `BSP_GENERAL_I2C_SDA` (h:104) |
| SCL | 48 | `BSP_GENERAL_I2C_SCL` (h:105) |
| Expander INT | 2 | `BSP_IO_EXPANDER_INT` (h:106) |

400 kHz (`h:107`). Carries the IO expander, the RTC and — via the expander — the
audio codecs.

Devices on this bus:

| Device | Address | Source |
| --- | --- | --- |
| PCA9535 16-bit expander | `0x21` (`0b0100001`) | `c:164`, `esp_io_expander_pca95xx_16bit.h:91` |
| PCF8563 RTC | `0x51` | `DRV_PCF8563_I2C_ADDR` (h:149) |
| ES8311 speaker codec | `0x30` | `DRV_ES8311_I2C_ADDR` (h:159) |
| ES7243 / ES7243E mic codec | `0x13` / `0x14` | `DRV_ES7243_I2C_ADDR`, `DRV_ES7243E_I2C_ADDR` (h:160-161) |

### I2S0 — audio

| Signal | GPIO | Source |
| --- | --- | --- |
| MCLK | 10 | `BSP_AUDIO_I2S_MCLK` (h:111) |
| SCLK (BCLK) | 11 | `BSP_AUDIO_I2S_SCLK` (h:112) |
| LRCK (WS) | 12 | `BSP_AUDIO_I2S_LRCK` (h:113) |
| DIN (from mic) | 15 | `BSP_AUDIO_I2S_DSIN` (h:114) |
| DOUT (to speaker) | 16 | `BSP_AUDIO_I2S_DOUT` (h:115) |

16 kHz / 16-bit / mono defaults (`h:162-166`), mic gain 27 dB.

### Wheel ("knob")

| Signal | GPIO | Source |
| --- | --- | --- |
| Encoder A | 41 | `BSP_KNOB_A` (h:64) |
| Encoder B | 42 | `BSP_KNOB_B` (h:65) |
| Encoder button | expander P0.3 | `BSP_KNOB_BTN` (h:66) |

Driven by the `knob` component; long-press/release callbacks via
`bsp_set_btn_long_press_cb()` / `bsp_set_btn_long_release_cb()` (h:303-304).

### Other

| Function | GPIO | Source | Notes |
| --- | --- | --- | --- |
| RGB LED data | 40 | `BSP_RGB_CTRL` (h:56) | `led_strip`, 1 × addressable RGB |
| Battery ADC | 3 | `BSP_BAT_ADC_CHAN` (h:59) | ADC**1** channel 2 (`c:385`), 2.5 dB atten, divider ratio (62+20)/20 ≈ 4.1 (h:60-61), with calibration (`c:396-398`) |
| Himax flasher UART | TX 17, RX 18 | `BSP_SSCMA_FLASHER_UART_*` (h:78-80) | UART1 @ 921600 — flashing the HX6538 from the ESP32 |
| Alarm UART | *not stated* | wiki | Output on a connector on the back of the device. See `wiki/.../Notifications/watcher_uart_output.md`; the wiring is documented but not the GPIO numbers. |

## IO expander map (I2C0, 16-bit PCA9535)

Not a TCA9554 — a **16-bit PCA9535 at I2C address `0x21`**
(`ESP_IO_EXPANDER_I2C_PCA9535_ADDRESS_001 = 0b0100001`, `c:164`).

The BSP names ports with `IO_EXPANDER_PIN_NUM_0..15`, which is the IDF's bit
mask (`1 << n`) — so **n is a linear pin index**. Direction comes from two
masks (`h:146-147`, applied `c:166-167`):

- **Inputs:** `0x20FF` → bits 0–7 and bit 13
- **Outputs:** `0xDF00` → bits 8–15 except bit 13

| Bit | Net | Source | Direction |
| --- | --- | --- | --- |
| 0 | Charge detect | `BSP_PWR_CHRG_DET` (h:118) | in |
| 1 | Standby detect | `BSP_PWR_STDBY_DET` (h:119) | in |
| 2 | VBUS in detect | `BSP_PWR_VBUS_IN_DET` (h:120) | in |
| 3 | Knob button | `BSP_KNOB_BTN` (h:66) | in |
| 4 | microSD detect | `BSP_SD_GPIO_DET` (h:86) | in |
| 5 | Touch interrupt | `BSP_TOUCH_GPIO_INT` (h:97) | in |
| 6 | Himax SPI SYNC | `BSP_SSCMA_CLIENT_SPI_SYNC` (h:74-75) | in (SYNC is an input to the ESP32) |
| 7 | Himax reset | `BSP_SSCMA_CLIENT_RST` (h:69-70) | in?? — see note |
| 8 | microSD power | `BSP_PWR_SDCARD` (h:121) | out |
| 9 | LCD power | `BSP_PWR_LCD` (h:122) | out |
| 10 | System power | `BSP_PWR_SYSTEM` (h:123) | out — **dropping this powers the device off** (`c:350-353`) |
| 11 | Himax / AI chip power | `BSP_PWR_AI_CHIP` (h:124) | out |
| 12 | Codec / speaker PA power | `BSP_PWR_CODEC_PA` (h:125) | out |
| 13 | Battery present | `BSP_PWR_BAT_DET` (h:126) | **in** — read by `bsp_battery_is_present()` (`c:365-368`) |
| 14 | Grove power | `BSP_PWR_GROVE` (h:127) | out |
| 15 | Battery ADC enable | `BSP_PWR_BAT_ADC` (h:128) | out |

> **Two things to check against the schematic before trusting the port/pin
> columns.** First, the header's own comments label the masks "P0.0 ~ P0.7 |
> P1.3" / "P1.0 ~ P1.7 & ~P1.3", which does not line up with a `1 << n`
> reading (bit 13 would be "P1.5" in datasheet terms). The comments appear
> loose; the bit numbers are what the code actually uses. Second, `RST` (bit 7)
> and `SYNC` (bit 6) are both declared with a `_USE_EXPANDER true` flag
> (`h:70`, `h:75`), but bit 7 is inside the **input** mask — so an actual reset
> drive must come from somewhere else (schematic) or the mask comment is
> wrong. Do not assume; verify.

## Power-on sequence (from `bsp_io_expander_init()`, `c:145-181`)

1. Initialise I2C0.
2. Create the PCA95xx expander (`c:164`).
3. Set directions from the input/output masks.
4. Drive **all outputs to 0**, then raise **`BSP_PWR_SYSTEM`** first (`c:168-169`).
5. Wait 100 ms, then raise `BSP_PWR_START_UP` = SDCARD | LCD | SYSTEM |
   AI_CHIP | CODEC_PA | GROVE | BAT_ADC (`h:175`, `c:171`).
6. Wait 50 ms and log the whole expander state.

## Deep sleep and wake (`c:329-343`)

- Wake source is **`ext0` on GPIO2 (expander `INT`), level 0** — the only
  RTC-capable wake the board offers. Because `INT` is the expander's shared
  open-drain interrupt, any configured input transition can wake it — the knob
  button, touch, or charge/standby detect (`c:334-342`).
- Optional **timer wake** (`time_in_sec > 0`).
- Before sleeping, these rails are dropped: SDCARD, CODEC_PA, GROVE, BAT_ADC,
  LCD, AI_CHIP (`c:334`).
- There is **no USB or UART wake** documented. `BSP_PWR_SYSTEM` stays up, so the
  device is on the backup battery until the power button hole is pressed.

Contrast with the Waveshare board, where touch is on a dedicated RTC GPIO (16)
and the boot button must never be a wake source.
