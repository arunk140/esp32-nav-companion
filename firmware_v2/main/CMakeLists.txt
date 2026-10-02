idf_component_register(
  SRCS
    "main.c"
    "board.c"
    "lcd.c"
    "touch.c"
    "sdcard.c"
    "display.c"
    "settings.c"
    "tiles.c"
    "map_view.c"
    "ui_pair.c"
    "ble_nav.c"
    "media_art.c"
    "media_view.c"
    "ui_common.c"
  INCLUDE_DIRS "."
  REQUIRES
    esp_driver_i2c esp_driver_spi esp_driver_gpio esp_driver_ledc esp_driver_sdmmc
    esp_lcd fatfs sdmmc nvs_flash bt esp_timer esp_psram
)

# Hot per-pixel loop of the rotated (heading-up) map renderer.
set_source_files_properties(map_view.c PROPERTIES COMPILE_OPTIONS "-O3")
