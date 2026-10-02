#include "ui_pair.h"
#include "ble_service.h"
#include "settings.h"
#include "esp_mac.h"
#include <stdio.h>
#include <string.h>

void ui_pair_create(lv_obj_t *parent, lv_event_cb_t on_browse)
{
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "Scan to pair");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 52);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    char payload[160];
    int n = snprintf(payload, sizeof(payload),
                     "{\"n\":\"%s\",\"m\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"s\":\"%s\",\"t\":\"%s\"}",
                     settings_device_name(), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                     NAV_SVC_UUID_STR, settings_token_str());

    lv_obj_t *qr = lv_qrcode_create(parent);
    lv_qrcode_set_size(qr, 220);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 8, 0);
    lv_qrcode_update(qr, payload, (uint32_t)n);
    lv_obj_align(qr, LV_ALIGN_CENTER, 0, -6);

    lv_obj_t *name = lv_label_create(parent);
    lv_label_set_text_fmt(name, "%s  -  open the Nav app", settings_device_name());
    lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(0xB0B8C0), 0);
    lv_obj_align(name, LV_ALIGN_CENTER, 0, 130);

    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 170, 46);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 180);
    lv_obj_set_style_radius(btn, 23, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A6FDB), 0);
    lv_obj_add_event_cb(btn, on_browse, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "Browse map");
    lv_obj_center(bl);
}
