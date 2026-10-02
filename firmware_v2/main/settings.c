#include "settings.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <stdlib.h>

#define NS "nav"
#define KEY_BRIGHTNESS "brightness"
#define KEY_TOKEN "token"
#define KEY_PAIRED "paired"
#define KEY_NAME "devname"
#define KEY_LAST_LAT "last_lat"
#define KEY_LAST_LON "last_lon"
#define KEY_HAS_POS "has_pos"
#define KEY_ZOOM "zoom"

static char s_name[16];
static char s_token[8];
static bool s_paired;
static uint8_t s_brightness = NAV_BRIGHTNESS_DEFAULT;
static uint8_t s_zoom;
static bool s_has_pos;
static int32_t s_lat, s_lon;

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    nvs_handle_t h;
    err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    size_t len = sizeof(s_name);
    if (nvs_get_str(h, KEY_NAME, s_name, &len) != ESP_OK || !s_name[0]) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_BT);
        snprintf(s_name, sizeof(s_name), "NAV-%02X%02X", mac[4], mac[5]);
        nvs_set_str(h, KEY_NAME, s_name);
    }

    uint32_t tok = 0;
    if (nvs_get_u32(h, KEY_TOKEN, &tok) != ESP_OK || tok < 100000 || tok > 999999) {
        tok = 100000 + (esp_random() % 900000);
        nvs_set_u32(h, KEY_TOKEN, tok);
    }
    snprintf(s_token, sizeof(s_token), "%06lu", (unsigned long)tok);

    uint8_t v = 0;
    if (nvs_get_u8(h, KEY_PAIRED, &v) == ESP_OK) s_paired = v != 0;
    if (nvs_get_u8(h, KEY_BRIGHTNESS, &v) == ESP_OK && v <= 100) s_brightness = v;
    if (nvs_get_u8(h, KEY_ZOOM, &v) == ESP_OK) s_zoom = v;
    if (nvs_get_u8(h, KEY_HAS_POS, &v) == ESP_OK && v) {
        s_has_pos = nvs_get_i32(h, KEY_LAST_LAT, &s_lat) == ESP_OK &&
                    nvs_get_i32(h, KEY_LAST_LON, &s_lon) == ESP_OK;
    }
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

static void put_u8(const char *key, uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

const char *settings_device_name(void)
{
    return s_name;
}

const char *settings_token_str(void)
{
    return s_token;
}

void settings_new_token(void)
{
    uint32_t old = (uint32_t)strtoul(s_token, NULL, 10);
    uint32_t tok;
    do {
        tok = 100000 + (esp_random() % 900000);
    } while (tok == old);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, KEY_TOKEN, tok);
    nvs_commit(h);
    nvs_close(h);
    snprintf(s_token, sizeof(s_token), "%06lu", (unsigned long)tok);
}

bool settings_paired(void)
{
    return s_paired;
}

void settings_set_paired(bool paired)
{
    if (paired == s_paired) return;
    s_paired = paired;
    put_u8(KEY_PAIRED, paired ? 1 : 0);
}

uint8_t settings_brightness(void)
{
    return s_brightness;
}

void settings_set_brightness(uint8_t pct)
{
    if (pct > 100) pct = 100;
    if (pct == s_brightness) return;
    s_brightness = pct;
    put_u8(KEY_BRIGHTNESS, pct);
}

uint8_t settings_zoom(void)
{
    return s_zoom;
}

void settings_set_zoom(uint8_t z)
{
    if (z == s_zoom) return;
    s_zoom = z;
    put_u8(KEY_ZOOM, z);
}

bool settings_last_pos(int32_t *lat_e7, int32_t *lon_e7)
{
    if (!s_has_pos) return false;
    *lat_e7 = s_lat;
    *lon_e7 = s_lon;
    return true;
}

void settings_set_last_pos(int32_t lat_e7, int32_t lon_e7)
{
    if (s_has_pos && lat_e7 == s_lat && lon_e7 == s_lon) return;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, KEY_LAST_LAT, lat_e7);
    nvs_set_i32(h, KEY_LAST_LON, lon_e7);
    nvs_set_u8(h, KEY_HAS_POS, 1);
    nvs_commit(h);
    nvs_close(h);
    s_has_pos = true;
    s_lat = lat_e7;
    s_lon = lon_e7;
}
