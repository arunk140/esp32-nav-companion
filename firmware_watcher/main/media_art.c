#include "media_art.h"
#include "ble_service.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jpeg_decoder.h"
#include "sdkconfig.h"
#include <math.h>
#include <string.h>

static const char *TAG = "art";

#define PX NAV_ART_PX
#define IMG_BYTES (PX * PX * 2)
#define MB_BYTES (12 * 1024)

static MessageBufferHandle_t s_mb;  // host task -> art task, one ART write each
static QueueHandle_t s_done_q;      // art task -> GUI, buffer index
static SemaphoreHandle_t s_ack;     // GUI -> art task: front buffer switched
static uint8_t *s_jpeg;             // reassembly buffer
static uint16_t *s_img[2];
static lv_image_dsc_t s_dsc[2];
static uint16_t s_img_id[2];
static volatile int s_front = -1;
static volatile uint16_t s_rx_id;
static volatile int64_t s_rx_last_us;

static inline uint16_t le16(const uint8_t *b)
{
    return (uint16_t)(b[0] | (b[1] << 8));
}

static inline uint32_t le32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static inline uint16_t blend565(uint16_t a, uint16_t b, uint32_t k /* 0..256 weight of a */)
{
    uint32_t r = (((a >> 11) & 0x1F) * k + ((b >> 11) & 0x1F) * (256 - k)) >> 8;
    uint32_t g = (((a >> 5) & 0x3F) * k + ((b >> 5) & 0x3F) * (256 - k)) >> 8;
    uint32_t bl = ((a & 0x1F) * k + (b & 0x1F) * (256 - k)) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

// Round the image once, here, so drawing it later is a plain copy.
static void apply_round_mask(uint16_t *img)
{
    const uint16_t bg = lv_color_to_u16(lv_color_hex(MEDIA_ART_BG));
    const float c = (PX - 1) / 2.0f, r = PX / 2.0f;
    for (int y = 0; y < PX; y++) {
        float dy = y - c;
        for (int x = 0; x < PX; x++) {
            float dx = x - c;
            float d2 = dx * dx + dy * dy;
            if (d2 <= (r - 1) * (r - 1)) continue;
            uint16_t *p = &img[y * PX + x];
            if (d2 >= r * r) {
                *p = bg;
            } else {
                float k = r - sqrtf(d2);  // 0..1 coverage across the edge pixel
                *p = blend565(*p, bg, (uint32_t)(k * 256));
            }
        }
    }
}

static bool decode(size_t len, int target)
{
    esp_jpeg_image_cfg_t cfg = {
        .indata = s_jpeg,
        .indata_size = len,
        .outbuf = (uint8_t *)s_img[target],
        .outbuf_size = IMG_BYTES,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
    };
    esp_jpeg_image_output_t out;
    if (esp_jpeg_get_image_info(&cfg, &out) != ESP_OK || out.width != PX || out.height != PX) {
        ESP_LOGW(TAG, "art rejected: not a %dx%d baseline JPEG", PX, PX);
        return false;
    }
    if (esp_jpeg_decode(&cfg, &out) != ESP_OK) {
        ESP_LOGW(TAG, "art decode failed");
        return false;
    }
    apply_round_mask(s_img[target]);
    return true;
}

static void art_task(void *arg)
{
    (void)arg;
    static uint8_t msg[256];
    uint16_t id = 0;
    uint32_t total = 0, got = 0;
    for (;;) {
        size_t n = xMessageBufferReceive(s_mb, msg, sizeof(msg), portMAX_DELAY);
        if (n < NAV_MEDIA_ART_HDR) continue;
        uint16_t cid = le16(msg + 1);
        uint32_t off = le32(msg + 3), tot = le32(msg + 7);
        size_t dlen = n - NAV_MEDIA_ART_HDR;
        if (off == 0) {
            id = cid;
            total = tot;
            got = 0;
        }
        // Ignore anything that doesn't continue the current transfer exactly.
        if (cid != id || tot != total || off != got || total == 0 ||
            total > NAV_ART_MAX_BYTES || got + dlen > total) {
            id = 0;
            continue;
        }
        memcpy(s_jpeg + got, msg + NAV_MEDIA_ART_HDR, dlen);
        got += dlen;
        if (got < total) continue;

        int target = s_front == 0 ? 1 : 0;
        int64_t t0 = esp_timer_get_time();
        bool ok = decode(total, target);
#if CONFIG_NAV_DEBUG_BLE
        ESP_LOGW(TAG, "art %04x: %lu B jpeg, decode %s in %lld ms", id, (unsigned long)total,
                 ok ? "ok" : "FAILED", (esp_timer_get_time() - t0) / 1000);
#else
        (void)t0;
#endif
        if (ok) {
            s_img_id[target] = id;
            xQueueOverwrite(s_done_q, &target);
            // Don't touch the other buffer until the GUI has switched to it.
            xSemaphoreTake(s_ack, pdMS_TO_TICKS(2000));
        }
        id = 0;
    }
}

void media_art_init(void)
{
    s_mb = xMessageBufferCreateWithCaps(MB_BYTES, MALLOC_CAP_SPIRAM);
    s_done_q = xQueueCreate(1, sizeof(int));
    s_ack = xSemaphoreCreateBinary();
    s_jpeg = heap_caps_malloc(NAV_ART_MAX_BYTES, MALLOC_CAP_SPIRAM);
    for (int i = 0; i < 2; i++) {
        s_img[i] = heap_caps_aligned_alloc(64, IMG_BYTES, MALLOC_CAP_SPIRAM);
        s_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        s_dsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
        s_dsc[i].header.w = PX;
        s_dsc[i].header.h = PX;
        s_dsc[i].header.stride = PX * 2;
        s_dsc[i].data_size = IMG_BYTES;
        s_dsc[i].data = (const uint8_t *)s_img[i];
    }
    if (!s_mb || !s_jpeg || !s_img[0] || !s_img[1]) {
        ESP_LOGE(TAG, "album art OOM");
        return;
    }
    // Decoding is short and bursty: lowest priority, core 0 (GUI owns core 1).
    xTaskCreatePinnedToCore(art_task, "art", 6144, NULL, 1, NULL, 0);
}

void media_art_feed(const uint8_t *data, size_t len)
{
    if (!s_mb || len < NAV_MEDIA_ART_HDR) return;
    s_rx_id = le16(data + 1);
    s_rx_last_us = esp_timer_get_time();
    // Never block the host task; a dropped chunk just abandons this image.
    xMessageBufferSend(s_mb, data, len, 0);
}

bool media_art_poll(const lv_image_dsc_t **img, uint16_t *art_id)
{
    int idx;
    if (!s_done_q || xQueueReceive(s_done_q, &idx, 0) != pdTRUE) return false;
    s_front = idx;
    *img = &s_dsc[idx];
    *art_id = s_img_id[idx];
    xSemaphoreGive(s_ack);
    return true;
}

uint16_t media_art_rx_id(void)
{
    return s_rx_id;
}

uint32_t media_art_rx_idle_ms(void)
{
    return (uint32_t)((esp_timer_get_time() - s_rx_last_us) / 1000);
}
