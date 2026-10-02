#include "lcd.h"
#include "board.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "lcd";

#define LCD_SPI_HOST SPI2_HOST
#define LCD_SPI_MOSI 1
#define LCD_SPI_SCLK 2

#define PIN_PCLK 41
#define PIN_HSYNC 38
#define PIN_VSYNC 39
#define PIN_DE 40

#define BOUNCE_LINES 20

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_frame_sem;
static volatile bool s_frame_armed;

// Vendor init sequence for the 2.8" panel (docs/waveshare-2.8c/examples,
// ST7701S_screen_init type 1). Format: cmd, n, data[n]; delay entries use
// cmd 0x00 with the delay in ms as the single data byte.
static const uint8_t st7701s_init[] = {
    0xFF, 5, 0x77, 0x01, 0x00, 0x00, 0x13,
    0xEF, 1, 0x08,
    0xFF, 5, 0x77, 0x01, 0x00, 0x00, 0x10,
    0xC0, 2, 0x3B, 0x00,
    0xC1, 2, 0x10, 0x0C,
    0xC2, 2, 0x07, 0x0A,
    0xC7, 1, 0x00,
    0xCC, 1, 0x10,
    0xCD, 1, 0x08,
    0xB0, 16, 0x05, 0x12, 0x98, 0x0E, 0x0F, 0x07, 0x07, 0x09, 0x09, 0x23, 0x05, 0x52, 0x0F, 0x67, 0x2C, 0x11,
    0xB1, 16, 0x0B, 0x11, 0x97, 0x0C, 0x12, 0x06, 0x06, 0x08, 0x08, 0x22, 0x03, 0x51, 0x11, 0x66, 0x2B, 0x0F,
    0xFF, 5, 0x77, 0x01, 0x00, 0x00, 0x11,
    0xB0, 1, 0x5D,
    0xB1, 1, 0x3E,
    0xB2, 1, 0x81,
    0xB3, 1, 0x80,
    0xB5, 1, 0x4E,
    0xB7, 1, 0x85,
    0xB8, 1, 0x20,
    0xC1, 1, 0x78,
    0xC2, 1, 0x78,
    0xD0, 1, 0x88,
    0xE0, 3, 0x00, 0x00, 0x02,
    0xE1, 11, 0x06, 0x30, 0x08, 0x30, 0x05, 0x30, 0x07, 0x30, 0x00, 0x33, 0x33,
    0xE2, 12, 0x11, 0x11, 0x33, 0x33, 0xF4, 0x00, 0x00, 0x00, 0xF4, 0x00, 0x00, 0x00,
    0xE3, 4, 0x00, 0x00, 0x11, 0x11,
    0xE4, 2, 0x44, 0x44,
    0xE5, 16, 0x0D, 0xF5, 0x30, 0xF0, 0x0F, 0xF7, 0x30, 0xF0, 0x09, 0xF1, 0x30, 0xF0, 0x0B, 0xF3, 0x30, 0xF0,
    0xE6, 4, 0x00, 0x00, 0x11, 0x11,
    0xE7, 2, 0x44, 0x44,
    0xE8, 16, 0x0C, 0xF4, 0x30, 0xF0, 0x0E, 0xF6, 0x30, 0xF0, 0x08, 0xF0, 0x30, 0xF0, 0x0A, 0xF2, 0x30, 0xF0,
    0xE9, 2, 0x36, 0x01,
    0xEB, 7, 0x00, 0x01, 0xE4, 0xE4, 0x44, 0x88, 0x40,
    0xED, 16, 0xFF, 0x10, 0xAF, 0x76, 0x54, 0x2B, 0xCF, 0xFF, 0xFF, 0xFC, 0xB2, 0x45, 0x67, 0xFA, 0x01, 0xFF,
    0xEF, 6, 0x08, 0x08, 0x08, 0x45, 0x3F, 0x54,
    0xFF, 5, 0x77, 0x01, 0x00, 0x00, 0x00,
    0x11, 0,
    0x00, 1, 120,
    0x3A, 1, 0x66,
    0x36, 1, 0x00,
    0x35, 1, 0x00,
    0x29, 0,
};

static void spi_write9(spi_device_handle_t dev, bool is_data, uint8_t v)
{
    spi_transaction_t t = {
        .cmd = is_data ? 1 : 0,
        .addr = v,
    };
    spi_device_polling_transmit(dev, &t);
}

static esp_err_t send_init_sequence(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = LCD_SPI_MOSI,
        .sclk_io_num = LCD_SPI_SCLK,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
    };
    esp_err_t err = spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_DISABLED);
    if (err != ESP_OK) return err;
    spi_device_interface_config_t devcfg = {
        .command_bits = 1,  // D/C bit (9-bit 3-wire SPI)
        .address_bits = 8,
        .clock_speed_hz = 4 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = -1,  // CS is on the expander
        .queue_size = 1,
    };
    spi_device_handle_t dev;
    err = spi_bus_add_device(LCD_SPI_HOST, &devcfg, &dev);
    if (err != ESP_OK) return err;

    board_exio_set(EXIO_LCD_CS, false);
    vTaskDelay(pdMS_TO_TICKS(5));
    size_t i = 0;
    while (i < sizeof(st7701s_init)) {
        uint8_t cmd = st7701s_init[i++];
        uint8_t n = st7701s_init[i++];
        if (cmd == 0x00) {
            vTaskDelay(pdMS_TO_TICKS(st7701s_init[i]));
        } else {
            spi_write9(dev, false, cmd);
            for (uint8_t k = 0; k < n; k++) spi_write9(dev, true, st7701s_init[i + k]);
        }
        i += n;
    }
    board_exio_set(EXIO_LCD_CS, true);

    // Hand GPIO1/2 back: the SD card (SDMMC CLK/CMD) uses the same pins.
    spi_bus_remove_device(dev);
    spi_bus_free(LCD_SPI_HOST);
    return ESP_OK;
}

static IRAM_ATTR bool on_frame_complete(esp_lcd_panel_handle_t panel,
                                        const esp_lcd_rgb_panel_event_data_t *ev,
                                        void *ctx)
{
    (void)panel;
    (void)ev;
    (void)ctx;
    BaseType_t woken = pdFALSE;
    if (s_frame_armed) {
        s_frame_armed = false;
        xSemaphoreGiveFromISR(s_frame_sem, &woken);
    }
    return woken == pdTRUE;
}

esp_err_t lcd_init(void)
{
    board_exio_set(EXIO_LCD_RST, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    board_exio_set(EXIO_LCD_RST, true);
    vTaskDelay(pdMS_TO_TICKS(120));

    esp_err_t err = send_init_sequence();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init sequence failed: %s", esp_err_to_name(err));
        return err;
    }

    s_frame_sem = xSemaphoreCreateBinary();

    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = CONFIG_NAV_LCD_PCLK_MHZ * 1000 * 1000,
            .h_res = BOARD_LCD_W,
            .v_res = BOARD_LCD_H,
            .hsync_pulse_width = 8,
            .hsync_back_porch = 10,
            .hsync_front_porch = 50,
            .vsync_pulse_width = 2,
            .vsync_back_porch = 18,
            .vsync_front_porch = 8,
            .flags.pclk_active_neg = false,
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 3,  // triple buffering: LVGL never waits for vsync
        .bounce_buffer_size_px = BOUNCE_LINES * BOARD_LCD_W,
        .dma_burst_size = 64,
        .hsync_gpio_num = PIN_HSYNC,
        .vsync_gpio_num = PIN_VSYNC,
        .de_gpio_num = PIN_DE,
        .pclk_gpio_num = PIN_PCLK,
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            5, 45, 48, 47, 21,      // B0..B4
            14, 13, 12, 11, 10, 9,  // G0..G5
            46, 3, 8, 18, 17,       // R0..R4
        },
        .flags.fb_in_psram = true,
    };
    err = esp_lcd_new_rgb_panel(&cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rgb panel: %s", esp_err_to_name(err));
        return err;
    }
    esp_lcd_rgb_panel_event_callbacks_t cbs = {
        .on_frame_buf_complete = on_frame_complete,
    };
    esp_lcd_rgb_panel_register_event_callbacks(s_panel, &cbs, NULL);
    esp_lcd_panel_reset(s_panel);
    return esp_lcd_panel_init(s_panel);
}

esp_lcd_panel_handle_t lcd_panel(void)
{
    return s_panel;
}

void lcd_get_frame_buffers(void **fb0, void **fb1, void **fb2)
{
    esp_lcd_rgb_panel_get_frame_buffer(s_panel, 3, fb0, fb1, fb2);
}

void lcd_arm_frame_wait(void)
{
    xSemaphoreTake(s_frame_sem, 0);
    s_frame_armed = true;
}

bool lcd_wait_frame(uint32_t timeout_ms)
{
    return xSemaphoreTake(s_frame_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
