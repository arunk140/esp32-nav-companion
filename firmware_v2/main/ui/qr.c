#include "ui/qr.h"
#include "gfx/gfx.h"
#include "settings.h"
#include "ble/ble_service.h"
#include "qrcode.h"
#include "esp_mac.h"
#include <stdio.h>

#define QR_SCALE 4
#define QR_QUIET 4  // modules of margin; the spec wants 4

// Where to draw, handed to the generator's callback through user_data.
typedef struct {
    int cx, top;
    int dim;  // filled in: full drawn side in px
} qr_draw_ctx_t;

int qr_payload(char *out, int out_len)
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
    return (n > 0 && (size_t)n < (size_t)out_len) ? n : 0;
}

// The generator hands us the finished symbol, so we can ask its size and draw
// in the same pass -- no second encode, no cached bitmap.
static void qr_cb(esp_qrcode_handle_t qr, void *user)
{
    qr_draw_ctx_t *ctx = (qr_draw_ctx_t *)user;
    int n = esp_qrcode_get_size(qr);
    int side = (n + 2 * QR_QUIET) * QR_SCALE;
    ctx->dim = side;
    int x0 = ctx->cx - side / 2;
    int y0 = ctx->top;

    // White card with a border, then the modules. Scanners need the quiet
    // zone, and a plain white field keeps the contrast ratio high.
    gfx_fill_rect(x0 - 8, y0 - 8, side + 16, side + 16, GFX_RGB(0xff, 0xff, 0xff));
    gfx_rect_outline(x0 - 8, y0 - 8, side + 16, side + 16, GFX_RGB(0x33, 0x33, 0x33));
    for (int my = -QR_QUIET; my < n + QR_QUIET; my++) {
        for (int mx = -QR_QUIET; mx < n + QR_QUIET; mx++) {
            // Out-of-range coordinates read as white, which is exactly the
            // quiet zone we want.
            if (!esp_qrcode_get_module(qr, mx, my)) continue;
            gfx_fill_rect(x0 + (mx + QR_QUIET) * QR_SCALE,
                          y0 + (my + QR_QUIET) * QR_SCALE, QR_SCALE, QR_SCALE,
                          GFX_RGB(0x00, 0x00, 0x00));
        }
    }
    gfx_dirty(x0 - 8, y0 - 8, side + 16, side + 16);
}

esp_err_t qr_draw(int cx, int top)
{
    static qr_draw_ctx_t ctx;  // outlives the callback
    char payload[224];
    if (!qr_payload(payload, sizeof(payload))) return ESP_FAIL;
    ctx.cx = cx;
    ctx.top = top;
    ctx.dim = 0;

    esp_qrcode_config_t cfg = {
        .display_func_with_cb = qr_cb,
        .max_qrcode_version = 10,  // <= 57 modules; this payload needs far less
        .qrcode_ecc_level = ESP_QRCODE_ECC_MED,
        .user_data = &ctx,
    };
    esp_err_t err = esp_qrcode_generate(&cfg, payload);
    if (err == ESP_OK && ctx.dim == 0) err = ESP_FAIL;  // callback never ran
    return err;
}
