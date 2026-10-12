#include "lcd.h"
#include "board.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_spd2010.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "lcd";

// QSPI wires, from docs-watcher/pinout.md. Note D1 is GPIO1, which the vendor
// header also names as the (unused) LCD DC pin — in quad mode dc_gpio_num is
// -1, so only the QSPI role is live.
#define LCD_HOST SPI3_HOST
#define LCD_CS 45
#define LCD_PCLK 7
#define LCD_D0 9
#define LCD_D1 1
#define LCD_D2 14
#define LCD_D3 13

// The QSPI bus's max_transfer_sz bounds BOTH the per-transaction length AND how
// many DMA descriptors the driver must reserve for it. Too large and a
// full-frame draw exhausts the descriptor pool: spi_device_queue_trans() then
// fails, surfacing as "spi transmit (queue) color failed" on every frame even
// though the panel's tiny init commands went through fine.
//
// The vendor BSP splits one frame into 16 chunks
// (CONFIG_BSP_LCD_SPI_DMA_SIZE_DIV=16, i.e. 412*412*2/16 = 21218 bytes) and runs
// a transaction queue depth of 1 (CONFIG_BSP_LCD_PANEL_SPI_TRANS_Q_DEPTH=1).
// Both values are copied here; they are the configuration known to work on this
// panel, and the earlier "obvious" choice of one transaction per frame does not.
#define LCD_DMA_SIZE_DIV 16
#define LCD_MAX_TRANSFER_BYTES (BOARD_LCD_W * BOARD_LCD_H * (int)sizeof(uint16_t) / LCD_DMA_SIZE_DIV)
#define LCD_TRANS_QUEUE_DEPTH 1

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;

esp_err_t lcd_init(void)
{
    const spi_bus_config_t buscfg = SPD2010_PANEL_BUS_QSPI_CONFIG(
        LCD_PCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3, LCD_MAX_TRANSFER_BYTES);
    esp_err_t err = spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi bus: %s", esp_err_to_name(err));
        return err;
    }

    // Take the component's QSPI template (spi_mode 3, dc -1, quad_mode, 32-bit
    // command / 8-bit parameter framing) and only override the clock. The
    // template defaults to 20 MHz; the vendor BSP runs this panel at 40 MHz.
    esp_lcd_panel_io_spi_config_t io_config = SPD2010_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
    io_config.pclk_hz = CONFIG_NAV_LCD_QSPI_MHZ * 1000 * 1000;
    io_config.trans_queue_depth = LCD_TRANS_QUEUE_DEPTH;
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &s_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel io: %s", esp_err_to_name(err));
        return err;
    }

    const spd2010_vendor_config_t vendor_config = {
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        // Unconnected on this board: the rail is the reset. See lcd.h.
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_config,
    };
    err = esp_lcd_new_panel_spd2010(s_io, &panel_config, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel driver: %s", esp_err_to_name(err));
        return err;
    }

    // Same bring-up order as the vendor BSP (sensecap-watcher.c:639-644).
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    ESP_LOGW(TAG, "SPD2010 up: %dx%d QSPI @ %d MHz", BOARD_LCD_W, BOARD_LCD_H,
             CONFIG_NAV_LCD_QSPI_MHZ);
    return ESP_OK;
}

esp_lcd_panel_handle_t lcd_panel(void)
{
    return s_panel;
}

esp_lcd_panel_io_handle_t lcd_io(void)
{
    return s_io;
}
