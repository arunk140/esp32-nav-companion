#include "map/tile_src.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "tile_src";

#define SCRATCH_N 3
#define QUEUE_DEPTH 16

static QueueHandle_t s_req_q;
static QueueHandle_t s_done_q;
static QueueHandle_t s_free_q;
static volatile int s_active;

int tile_src_active(void)
{
    return s_active;
}

// Read one tile into dst. Returns false if the file is missing or short.
static bool read_tile(int z, int x, int y, uint8_t *dst)
{
    char path[96];
    snprintf(path, sizeof(path), "/sdcard/street_map/%d/%d/%d.bin", z, x, y);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = false;
    if (fseek(f, TILE_HEADER_BYTES, SEEK_SET) == 0) {
        size_t n = fread(dst, 1, TILE_BYTES, f);
        ok = (n == TILE_BYTES);
    }
    fclose(f);
    if (!ok) ESP_LOGD(TAG, "short/absent tile %d/%d/%d", z, x, y);
    return ok;
}

static void loader_task(void *arg)
{
    (void)arg;
    tile_req_t req;
    for (;;) {
        if (xQueueReceive(s_req_q, &req, portMAX_DELAY) != pdTRUE) continue;
        uint8_t *scratch = NULL;
        if (xQueueReceive(s_free_q, &scratch, portMAX_DELAY) != pdTRUE) continue;
        bool ok = read_tile(req.z, req.x, req.y, scratch);
        tile_done_t done = {
            .slot = req.slot, .z = req.z, .x = req.x, .y = req.y,
            .gen = req.gen, .ok = ok, .scratch = ok ? scratch : NULL,
        };
        if (!ok) {
            // Hand the buffer straight back: a missing tile must not be able
            // to starve the free pool and wedge every later load.
            xQueueSend(s_free_q, &scratch, 0);
        }
        // The done queue is sized for the worst case; drop rather than block
        // if it is full. The consumer's stall detector re-arms the slot.
        xQueueSend(s_done_q, &done, 0);
        s_active--;
    }
}

void tile_src_init(void)
{
    if (s_req_q) return;
    s_req_q = xQueueCreate(QUEUE_DEPTH, sizeof(tile_req_t));
    s_done_q = xQueueCreate(QUEUE_DEPTH, sizeof(tile_done_t));
    s_free_q = xQueueCreate(SCRATCH_N, sizeof(uint8_t *));
    for (int i = 0; i < SCRATCH_N; i++) {
        uint8_t *b = heap_caps_malloc(TILE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!b) {
            // Fatal to the map, but not to the pairing screen: carry on with
            // fewer buffers and let the map show gaps.
            ESP_LOGE(TAG, "scratch OOM (%d)", i);
            continue;
        }
        xQueueSend(s_free_q, &b, 0);
    }
    // Priority 2, core 0: below the GUI task, and on the other core so an SD
    // read never competes with rendering.
    xTaskCreatePinnedToCore(loader_task, "tile_ld", 4096, NULL, 2, NULL, 0);
    ESP_LOGI(TAG, "loader up (%d scratch)", SCRATCH_N);
}

bool tile_src_enqueue(const tile_req_t *req)
{
    if (!s_req_q) return false;
    if (xQueueSend(s_req_q, req, 0) != pdTRUE) return false;
    s_active++;
    return true;
}

bool tile_src_poll(tile_done_t *out)
{
    if (!s_done_q || !out) return false;
    return xQueueReceive(s_done_q, out, 0) == pdTRUE;
}

void tile_src_release(const uint8_t *scratch)
{
    if (!scratch || !s_free_q) return;
    uint8_t *m = (uint8_t *)scratch;
    xQueueSend(s_free_q, &m, 0);
}
