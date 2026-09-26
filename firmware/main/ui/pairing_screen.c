#include "ui/pairing_screen.h"
#include "settings.h"
#include "ble/ble_service.h"  // NAV_SVC_UUID_STR for the QR payload
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "qrcode.h"
#include <stdio.h>
#include <string.h>

#define QR_SCALE 4
#define QR_MARGIN 4

bool pairing_build_payload(char *out, size_t out_len)
{
    const char *name = settings_get_device_name();
    uint32_t token = settings_get_token();
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    int n = snprintf(out, out_len,
                     "{\"n\":\"%s\",\"m\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
                     "\"s\":\"%s\",\"t\":\"%06lu\"}",
                     name, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                     NAV_SVC_UUID_STR, (unsigned long)token);
    return n > 0 && (size_t)n < out_len;
}

typedef struct {
    lv_obj_t *canvas;
    int dim;    // canvas side in px
    int size;   // actual QR modules per side (filled by callback)
} qr_draw_t;

// Caller skip callback + arg (single pairing screen at a time).
static pairing_skip_cb_t s_skip_cb;
static void *s_skip_arg;

static void skip_evt(lv_event_t *e)
{
    (void)e;
    if (s_skip_cb) s_skip_cb(s_skip_arg);
}

static void qr_display_cb(esp_qrcode_handle_t qr, void *user_data)
{
    qr_draw_t *q = user_data;
    int n = esp_qrcode_get_size(qr);
    q->size = n;
    // Center the drawn code inside the (larger, worst-case) canvas.
    int drawn = (n + QR_MARGIN * 2) * QR_SCALE;
    int off = (q->dim - drawn) / 2;
    for (int y = -QR_MARGIN; y < n + QR_MARGIN; y++) {
        for (int x = -QR_MARGIN; x < n + QR_MARGIN; x++) {
            bool black = esp_qrcode_get_module(qr, x, y);
            lv_color_t c = black ? lv_color_black() : lv_color_white();
            for (int dy = 0; dy < QR_SCALE; dy++) {
                for (int dx = 0; dx < QR_SCALE; dx++) {
                    int px = off + (x + QR_MARGIN) * QR_SCALE + dx;
                    int py = off + (y + QR_MARGIN) * QR_SCALE + dy;
                    if (px < q->dim && py < q->dim) {
                        lv_canvas_set_px(q->canvas, px, py, c, LV_OPA_COVER);
                    }
                }
            }
        }
    }
}

lv_obj_t *pairing_screen_create(lv_obj_t *parent, pairing_skip_cb_t on_skip, void *arg)
{
    lv_obj_t *scr = lv_obj_create(parent);
    lv_obj_set_size(scr, 480, 480);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    // Absolute layout (no flex): title clears the status dot at y=28..52.
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Scan to pair");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 52);

    // QR canvas: version <= 10 => <= 57 modules; (57+8)*5 = 325 px.
    // ~211 KB: must live in PSRAM, not .bss (internal RAM overflows the link).
    int dim = (57 + QR_MARGIN * 2) * QR_SCALE;
    lv_color_t *cbuf = heap_caps_malloc((size_t)dim * dim * sizeof(lv_color_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cbuf) return scr;  // OOM: title-only screen is better than a crash
    lv_obj_t *canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(canvas, cbuf, dim, dim, LV_COLOR_FORMAT_RGB565);
    lv_canvas_fill_bg(canvas, lv_color_white(), LV_OPA_COVER);
    lv_obj_align(canvas, LV_ALIGN_TOP_MID, 0, 95);  // 260px: spans y 95..355

    char payload[160];
    if (pairing_build_payload(payload, sizeof(payload))) {
        qr_draw_t q = {.canvas = canvas, .dim = dim, .size = 0};
        esp_qrcode_config_t cfg = {
            .display_func_with_cb = qr_display_cb,
            .max_qrcode_version = 10,
            .qrcode_ecc_level = ESP_QRCODE_ECC_LOW,
            .user_data = &q,
        };
        esp_qrcode_generate(&cfg, payload);
    }

    lv_obj_t *name = lv_label_create(scr);
    lv_label_set_text_fmt(name, "%s", settings_get_device_name());
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 362);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "Scan QR in Nav app");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 384);

    // Browse-only mode: map with drag + pinch, no location. One-way for M1
    // (back via BOOT long-press unpair in M3).
    lv_obj_t *skip = lv_button_create(scr);
    lv_obj_set_size(skip, 170, 44);
    lv_obj_align(skip, LV_ALIGN_TOP_MID, 0, 408);
    lv_obj_t *sl = lv_label_create(skip);
    lv_label_set_text(sl, "Skip - browse map");
    lv_obj_center(sl);
    lv_obj_add_event_cb(skip, skip_evt, LV_EVENT_CLICKED, NULL);
    s_skip_cb = on_skip;
    s_skip_arg = arg;

    return scr;
}
