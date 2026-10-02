#include "sdcard.h"
#include "board.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd";

#define SD_CLK 2
#define SD_CMD 1
#define SD_D0 42

static bool s_ok;

static esp_err_t try_mount(int khz)
{
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,  // never touch the user's map card
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = khz;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = SD_CLK;
    slot.cmd = SD_CMD;
    slot.d0 = SD_D0;
    slot.d1 = -1;
    slot.d2 = -1;
    slot.d3 = -1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    sdmmc_card_t *card;
    return esp_vfs_fat_sdmmc_mount(SD_MOUNT, &host, &slot, &mount, &card);
}

esp_err_t sdcard_init(void)
{
    board_exio_set(EXIO_SD_D3, true);
    vTaskDelay(pdMS_TO_TICKS(10));
    esp_err_t err = try_mount(SDMMC_FREQ_HIGHSPEED);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mount @40MHz failed (%s), retry @20MHz", esp_err_to_name(err));
        err = try_mount(SDMMC_FREQ_DEFAULT);
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
