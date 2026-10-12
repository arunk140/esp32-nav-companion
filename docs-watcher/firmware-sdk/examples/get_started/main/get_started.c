/*
 * SPDX-FileCopyrightText: 2024 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "sensecap-watcher.h"

static const char *TAG = "Main";

lv_disp_t *lvgl_disp;
#define CANVAS_WIDTH DRV_LCD_H_RES
#define CANVAS_HEIGHT DRV_LCD_V_RES
static lv_color_t *cbuf;

static int s_retry_num = 0;
static EventGroupHandle_t s_wifi_event_group;
static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        bsp_rgb_set(0, 0, 3);
        if (s_retry_num < 10) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, BIT1);
            bsp_rgb_set(3, 0, 0);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, BIT0);
        bsp_rgb_set(0, 3, 0);
    }
}
static void wifi_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    // ESP_ERROR_CHECK(esp_wifi_set_default_wifi_sta_handlers());
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "CMW",
            .threshold.authmode = WIFI_AUTH_OPEN,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start());
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        BIT0 | BIT1,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY);
    if (bits & BIT0) {
        ESP_LOGI(TAG, "connected");
    } else if (bits & BIT1) {
        ESP_LOGI(TAG, "Failed to connect");
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

void app_main(void)
{
    // bsp_sdcard_init_default();
    bsp_io_expander_init();
    bsp_exp_io_set_level(BSP_PWR_AI_CHIP, 1);

    lvgl_disp = bsp_lvgl_init();
    assert(lvgl_disp != NULL);
    lv_indev_t * tp = NULL;
    while (1) {
        tp = lv_indev_get_next(tp);
        if(tp->driver->type == LV_INDEV_TYPE_POINTER) {
            break;
        }
    }
    assert(tp != NULL);
    lv_point_t point_last = {0, 0};
    cbuf = heap_caps_malloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR(CANVAS_WIDTH, CANVAS_HEIGHT), MALLOC_CAP_SPIRAM);
    lv_obj_t * canvas = lv_canvas_create(lv_scr_act());
    lv_canvas_set_buffer(canvas, cbuf, CANVAS_WIDTH, CANVAS_HEIGHT, LV_IMG_CF_TRUE_COLOR);
    lv_obj_center(canvas);
    lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_COVER);

    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = lv_palette_main(LV_PALETTE_ORANGE);
    lv_canvas_draw_text(canvas, 40, 100, 160, &label_dsc, "touch screen to draw");
    label_dsc.color = lv_palette_main(LV_PALETTE_BLUE);
    lv_canvas_draw_text(canvas, 40, 120, 170, &label_dsc, "press button to clear");
    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.radius = 5;
    while (1) {
        lv_indev_get_point(tp, &point_last);
        lvgl_port_lock(portMAX_DELAY);
        lv_canvas_draw_rect(canvas, point_last.x, point_last.y, 10, 10, &rect_dsc);
        lvgl_port_unlock();
        vTaskDelay(17 / portTICK_PERIOD_MS);
        if (bsp_exp_io_get_level(BSP_KNOB_BTN) == 0) {
            bsp_system_reboot();
        }
    }
}