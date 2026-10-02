#include "settings.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_mac.h"
#include "esp_random.h"
#include <stdio.h>

#define NAV_NVS_NS "nav"
#define KEY_BRIGHTNESS "brightness"
#define KEY_TOKEN "token"
#define KEY_PAIRED "paired"
#define KEY_NAME "devname"

#define BRIGHTNESS_DEFAULT 80

static char s_name[16] = {0};

esp_err_t settings_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    return ret;
}

uint8_t settings_get_brightness(void)
{
    nvs_handle_t h;
    uint8_t v = BRIGHTNESS_DEFAULT;
    if (nvs_open(NAV_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, KEY_BRIGHTNESS, &v);
        nvs_close(h);
    }
    if (v > 100) v = 100;
    return v;
}

esp_err_t settings_set_brightness(uint8_t pct)
{
    if (pct > 100) pct = 100;
    nvs_handle_t h;
    esp_err_t r = nvs_open(NAV_NVS_NS, NVS_READWRITE, &h);
    if (r != ESP_OK) return r;
    r = nvs_set_u8(h, KEY_BRIGHTNESS, pct);
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    return r;
}

uint32_t settings_get_token(void)
{
    nvs_handle_t h;
    uint32_t t = 0;
    if (nvs_open(NAV_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_get_u32(h, KEY_TOKEN, &t) != ESP_OK || t < 100000 || t > 999999) {
            t = 100000 + (esp_random() % 900000);
            nvs_set_u32(h, KEY_TOKEN, t);
            nvs_commit(h);
        }
        nvs_close(h);
    }
    return t;
}

const char *settings_get_device_name(void)
{
    if (s_name[0]) return s_name;
    nvs_handle_t h;
    size_t len = sizeof(s_name);
    if (nvs_open(NAV_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_get_str(h, KEY_NAME, s_name, &len) != ESP_OK) {
            uint8_t mac[6];
            esp_read_mac(mac, ESP_MAC_BT);
            snprintf(s_name, sizeof(s_name), "NAV-%02X%02X", mac[4], mac[5]);
            nvs_set_str(h, KEY_NAME, s_name);
            nvs_commit(h);
        }
        nvs_close(h);
    }
    if (!s_name[0]) snprintf(s_name, sizeof(s_name), "NAV-0000");
    return s_name;
}

bool settings_get_paired(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NAV_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, KEY_PAIRED, &v);
        nvs_close(h);
    }
    return v != 0;
}

esp_err_t settings_set_paired(bool paired)
{
    nvs_handle_t h;
    esp_err_t r = nvs_open(NAV_NVS_NS, NVS_READWRITE, &h);
    if (r != ESP_OK) return r;
    r = nvs_set_u8(h, KEY_PAIRED, paired ? 1 : 0);
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    return r;
}

esp_err_t settings_factory_unpair(void)
{
    return settings_set_paired(false);
}

#define KEY_LAST_LAT "last_lat"
#define KEY_LAST_LON "last_lon"
#define KEY_HAS_POS "has_pos"
#define KEY_ZOOM "zoom"
#define ZOOM_DEFAULT 16
#define ZOOM_MIN_S 15
#define ZOOM_MAX_S 17

uint8_t settings_get_zoom(void)
{
    nvs_handle_t h;
    uint8_t z = ZOOM_DEFAULT;
    if (nvs_open(NAV_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, KEY_ZOOM, &z);
        nvs_close(h);
    }
    if (z < ZOOM_MIN_S || z > ZOOM_MAX_S) z = ZOOM_DEFAULT;
    return z;
}

esp_err_t settings_set_zoom(uint8_t zoom)
{
    if (zoom < ZOOM_MIN_S) zoom = ZOOM_MIN_S;
    if (zoom > ZOOM_MAX_S) zoom = ZOOM_MAX_S;
    nvs_handle_t h;
    esp_err_t r = nvs_open(NAV_NVS_NS, NVS_READWRITE, &h);
    if (r != ESP_OK) return r;
    r = nvs_set_u8(h, KEY_ZOOM, zoom);
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    return r;
}

bool settings_get_last_pos(int32_t *lat_e7, int32_t *lon_e7)
{
    nvs_handle_t h;
    uint8_t has = 0;
    int32_t lat = 0, lon = 0;
    if (nvs_open(NAV_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    nvs_get_u8(h, KEY_HAS_POS, &has);
    if (has) {
        nvs_get_i32(h, KEY_LAST_LAT, &lat);
        nvs_get_i32(h, KEY_LAST_LON, &lon);
    }
    nvs_close(h);
    if (!has) return false;
    if (lat_e7) *lat_e7 = lat;
    if (lon_e7) *lon_e7 = lon;
    return true;
}

esp_err_t settings_set_last_pos(int32_t lat_e7, int32_t lon_e7)
{
    nvs_handle_t h;
    esp_err_t r = nvs_open(NAV_NVS_NS, NVS_READWRITE, &h);
    if (r != ESP_OK) return r;
    r = nvs_set_i32(h, KEY_LAST_LAT, lat_e7);
    if (r == ESP_OK) r = nvs_set_i32(h, KEY_LAST_LON, lon_e7);
    if (r == ESP_OK) r = nvs_set_u8(h, KEY_HAS_POS, 1);
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    return r;
}
