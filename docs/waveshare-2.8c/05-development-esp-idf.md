# 05 — ESP-IDF Development

## Toolchain versions

| Component | Version | Source |
| --- | --- | --- |
| ESP-IDF | **V5.5.2** (as used in the wiki screenshots) | Wiki states: *"select the ESP-IDF version that matches your board's example"* |
| Minimum IDF | **>= 4.4** | `examples/esp-idf/main/idf_component.yml` |
| LVGL | **`~8.2.0`** | `main/idf_component.yml` |
| Target | ESP32-S3 | |

```yaml
# examples/esp-idf/main/idf_component.yml
dependencies:
  idf: ">=4.4"
  lvgl/lvgl: "~8.2.0"
```

> The wiki's screenshots use **5.5.2**, but the manifest only requires 4.4. If you
> hit build problems, match the example's version rather than always chasing
> latest. Newer IDF has changed `esp_lcd` and RGB panel APIs more than once.

## Project layout

```
examples/esp-idf/
├── CMakeLists.txt
├── partitions.csv
├── sdkconfig.defaults
├── sdkconfig.defaults.esp32s3
├── README.md
└── main/
    ├── CMakeLists.txt
    ├── idf_component.yml
    ├── Kconfig.projbuild
    ├── main.c                  ← entry point, init order
    ├── BAT_Driver/             ← battery voltage (ADC1 ch3, GPIO4)
    ├── Buzzer/                 ← expander EXIO8
    ├── EXIO/                   ← TCA9554PWR expander
    ├── I2C_Driver/             ← shared I2C0 bus
    ├── LCD_Driver/             ← ST7701S RGB panel
    ├── LVGL_Driver/            ← LVGL port glue
    ├── LVGL_UI/                ← demo UI
    ├── PCF85063/               ← RTC
    ├── QMI8658/                ← IMU
    ├── SD_Card/                ← TF, 1-bit
    ├── Touch_Driver/           ← GT911 + esp_lcd_touch
    └── Wireless/               ← Wi-Fi scan
```

> The upstream package also vendors LVGL as an IDF component
> (`components/lvgl__lvgl/`, ~1,900 files). We did not copy that tree — the
> component manager fetches it from `idf_component.yml`. Download the archive
> yourself if you want an offline build:
> [`ESP32-S3-Touch-LCD-2.8C-Demo.zip`](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.8C/ESP32-S3-Touch-LCD-2.8C-Demo.zip)

Note the vendor's own README instruction for VS Code:

> "When VScode selects the project, it is noted that it is not selected to
> ESP-IDF, but to the project folder under ESP-IDF. Normally, the name is
> product-Test."

So open `ESP32-S3-Touch-LCD-2.8C-Test` as the project root, not its parent.

## Build configuration

### `sdkconfig.defaults`

```ini
CONFIG_LV_MEM_CUSTOM=y
CONFIG_LV_MEMCPY_MEMSET_STD=y
CONFIG_LV_USE_USER_DATA=y
CONFIG_LV_USE_CHART=y
CONFIG_LV_USE_PERF_MONITOR=y

CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y

CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y
CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE=64

CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240=y
CONFIG_ESP32S3_DEFAULT_CPU_FREQ_MHZ=240
```

| Setting | Why it is here |
| --- | --- |
| `FLASHSIZE_16MB` | The board has 16 MB |
| `PARTITION_TABLE_CUSTOM` | Uses `partitions.csv` |
| `DATA_CACHE_LINE_64B` / `SIZE=64` | **DMA alignment requirement** for the RGB panel in PSRAM. Do not change. |
| `DEFAULT_CPU_FREQ_240` | 240 MHz |
| `LV_USE_CHART` | Enables LVGL chart widgets |
| `LV_USE_PERF_MONITOR` | LVGL performance monitor — useful for a map renderer |
| `LV_USE_USER_DATA` | Per-object user data |

### `sdkconfig.defaults.esp32s3`

```ini
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
```

Octal PSRAM at 80 MHz, with instruction/rodata fetch from PSRAM enabled. The
vendor's comment:

> "Enabling the following configurations can help increase the PCLK frequency in
> the case when the Frame Buffer is allocated from the PSRAM and fetched by EDMA"

> **If you disable PSRAM without also moving the frame buffer to internal SRAM,
> the display will not work.** A 480 × 480 RGB565 buffer is 450 KiB and does not
> fit alongside everything else in 512 KB of internal SRAM. The board needs its
> 8 MB of PSRAM.

### `partitions.csv`

```csv
# Name,     Type, SubType, Offset,   Size, Flags
nvs,        data, nvs,      0x9000,  0x6000,
factory,0,0,        0x10000, 3M,
flash_test, data, fat,      ,        528K,
```

| Partition | Offset | Size | Purpose |
| --- | --- | --- | --- |
| `nvs` | `0x9000` | 24 KB | Non-volatile storage |
| `factory` | `0x10000` | **3 MB** | Application |
| `flash_test` | auto | 528 KB | **FAT filesystem for the flash "SD" test** |

> **Two things to note for a nav build.**
>
> 1. **The app partition is only 3 MB.** LVGL is big. A nav application with
>    embedded fonts, map assets and a GNSS parser can plausibly exceed 3 MB.
>    Check the build size early and be ready to grow this.
> 2. **`flash_test` is a FAT partition on the internal flash.** The demo's
>    "Flash Size" panel and `Flash_Searching()` exercise it. It is *not* a
>    general-purpose data store, but it does prove internal-flash filesystem
>    support. The vendor README also warns: *"If the compilation succeeds the
>    first time and the compilation fails during subsequent tests, decompress the
>    folder again and compile the new file"* — a sign the shipped example has
>    build-order fragility.

## Environment installation

The wiki documents the Espressif Installation Manager (EIM) route, using Windows
as the example. macOS and Linux users should follow the
[official ESP-IDF get-started guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html).

### Windows (EIM)

1. Download the installer from the
   [ESP-IDF Installation Manager](https://dl.espressif.com/dl/eim/) page
2. Use the **Offline Installer** tab: pick Windows and your IDF version. Two files
   download — the **offline package (`.zst`)** and the **installer (`.exe`)**
3. Run the installer; it auto-detects the offline package in the same directory.
   Click **Install from archive**
4. Choose the install path. **Avoid non-ASCII characters and spaces**
5. On success, **Finish installation** → **Install driver**

### VS Code + ESP-IDF extension

1. Install [Visual Studio Code](https://code.visualstudio.com/) — tick *Add "Open
   with Code" action to Windows Explorer file context menu*
2. Open Extensions (`Ctrl+Shift+X`)
3. Search for and install the
   [ESP-IDF extension](https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension)
4. Extension **≥ 2.0** auto-detects the EIM-installed ESP-IDF environment — no
   manual configuration required

## `main.c` — the reference init sequence

```c
void Driver_Loop(void *parameter) {
    while (1) {
        QMI8658_Loop();
        RTC_Loop();
        BAT_Get_Volts();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void Driver_Init(void) {
    Flash_Searching();
    BAT_Init();
    I2C_Init();
    PCF85063_Init();
    QMI8658_Init();
    EXIO_Init();                 // expander — after I2C, before LCD
    xTaskCreatePinnedToCore(Driver_Loop, "Other Driver task",
                            4096, NULL, 3, NULL, 0);   // core 0, prio 3
}

void app_main(void) {
    Wireless_Init();
    Driver_Init();
    LCD_Init();
    Touch_Init();
    SD_Init();
    LVGL_Init();
    Lvgl_Example1();

    // lv_demo_widgets();       // available if you want the full LVGL demo suite
    // lv_demo_keypad_encoder();
    // lv_demo_benchmark();
    // lv_demo_stress();
    // lv_demo_music();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
        lv_timer_handler();
    }
}
```

The vendor comment on the main loop is the guidance worth keeping:

> "The task running `lv_timer_handler` should have lower priority than that running
> `lv_tick_inc`"

> **`lv_tick_inc` is not called here.** The ESP-IDF LVGL port runs the tick from a
> FreeRTOS timer, and `lv_timer_handler()` is called every 10 ms. If you restructure
> the loop, keep the tick mechanism and the relative priorities intact — a starved
> tick makes touch and animations crawl for reasons that are hard to see.

> **The `Driver_Loop` task is explicitly pinned to core 0** (`xTaskCreatePinnedToCore(..., 0)`),
> leaving core 1 free. The vendor does not say why in a comment, but the obvious
> reading is that the RGB panel's DMA path is kept on an uncontended core — which
> is why the teardown ships both a bounce buffer and tear-effect semaphores. Keep
> low-priority polling work off core 1 and you will avoid DMA jitter and screen
> tearing. Treat the core-0 pin as a requirement, not an accident, and verify the
> binding for your IDF version rather than assuming.

## The other examples shipped

`examples/esp-idf/pytest_rgb_panel_lvgl.py` is a pytest/Expect test for the RGB
panel + LVGL combination. If you change display configuration, this is your
regression harness — see the upstream file in the demo archive.

## General ESP-IDF tutorials

The wiki links a 9-section IDF tutorial series. The two most relevant to a nav
device:

- [Section 6: FreeRTOS](https://docs.waveshare.com/ESP32-ESP-IDF-Tutorials/FreeRTOS)
- [Section 7: Peripherals](https://docs.waveshare.com/ESP32-ESP-IDF-Tutorials/Peripheral)

> Both the Arduino and IDF tutorial series use the **ESP32-S3-Zero** as their
> teaching board. The wiki says so, and warns to check your board's pinout. Do not
> copy pin numbers from the tutorials.
