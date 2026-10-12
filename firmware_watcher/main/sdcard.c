#include "sdcard.h"
#include "board.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd";

// The microSD hangs off SPI2 — the *same* bus as the Himax AI module, on its
// own chip select (46 vs 21). The nav firmware keeps the Himax rail
// (EXIO_PWR_AI) off so the module cannot drive MISO; see board.c.
//
// This differs from the Waveshare board, where the card was on a 1-bit SDMMC
// host and shared its CLK/CMD pins with the panel's init SPI. Here the panel is
// on SPI3, so there is no LCD/SD pin conflict at all.
#define SD_HOST SPI2_HOST
#define SD_CS 46
#define SD_SCLK 4
#define SD_MOSI 5
#define SD_MISO 6

static bool s_ok;

static esp_err_t try_mount(int khz)
{
    const esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,  // never touch the user's map card
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_HOST;
    host.max_freq_khz = khz;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SD_HOST;
    slot.gpio_cs = SD_CS;

    sdmmc_card_t *card;
    return esp_vfs_fat_sdspi_mount(SD_MOUNT, &host, &slot, &mount, &card);
}

esp_err_t sdcard_init(void)
{
    if (!board_exio_get(EXIO_SD_DET)) {
        // Not fatal on its own (the switch can be flaky), but worth saying: the
        // mount below is what actually decides.
        ESP_LOGW(TAG, "no card detected (expander bit %d)", EXIO_SD_DET);
    }

    const spi_bus_config_t bus = {
        .mosi_io_num = SD_MOSI,
        .miso_io_num = SD_MISO,
        .sclk_io_num = SD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 8 * 1024,
    };
    esp_err_t err = spi_bus_initialize(SD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi bus: %s", esp_err_to_name(err));
        return err;
    }

    // SPI-mode cards top out well below the 40 MHz the Waveshare board ran in
    // 1-bit SDMMC. 20 MHz is the usual ceiling; fall back to 10 MHz for older
    // cards. Tiles are read once and cached, so this is not on any hot path.
    err = try_mount(20000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mount @20MHz failed (%s), retry @10MHz", esp_err_to_name(err));
        err = try_mount(10000);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return err;
    }
    s_ok = true;
    return ESP_OK;
}

bool sdcard_ok(void)
{
    return s_ok;
}
