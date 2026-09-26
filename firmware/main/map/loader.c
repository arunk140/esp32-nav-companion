#include "map/loader.h"
#include "map_tiles.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "loader";

#define SCRATCH_N 4
#define QUEUE_DEPTH 16

static QueueHandle_t s_req_q;
static QueueHandle_t s_done_q;
static QueueHandle_t s_free_q;
static loader_map_handle_t s_map;

static void loader_task(void *arg)
{
    (void)arg;
    load_req_t req;
    while (1) {
        if (xQueueReceive(s_req_q, &req, portMAX_DELAY) != pdTRUE) continue;
        uint8_t *scratch = NULL;
        if (xQueueReceive(s_free_q, &scratch, portMAX_DELAY) != pdTRUE) continue;
        bool ok = map_tiles_load_file(s_map, req.z, req.x, req.y, scratch);
        load_done_t done = {
            .slot = req.slot, .z = req.z, .x = req.x, .y = req.y,
            .gen = req.gen, .ok = ok, .scratch = ok ? scratch : NULL,
        };
        if (!ok) {
            // Scratch unused: return it immediately so a missing tile can't
            // wedge the free pool.
            xQueueSend(s_free_q, &scratch, 0);
            done.scratch = NULL;
        }
        // Done queue is sized for worst case; drop (don't block) if full —
        // the tile just pops in on a later re-request.
        xQueueSend(s_done_q, &done, 0);
    }
}

void loader_init(loader_map_handle_t handle)
{
    s_map = handle;
    s_req_q = xQueueCreate(QUEUE_DEPTH, sizeof(load_req_t));
    s_done_q = xQueueCreate(QUEUE_DEPTH, sizeof(load_done_t));
    s_free_q = xQueueCreate(SCRATCH_N, sizeof(uint8_t *));
    for (int i = 0; i < SCRATCH_N; i++) {
        uint8_t *b = heap_caps_malloc(LOADER_TILE_BYTES,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        // OOM here is fatal to the map; log loudly (boot will show gaps).
        if (!b) {
            ESP_LOGE(TAG, "scratch OOM");
            continue;
        }
        xQueueSend(s_free_q, &b, 0);
    }
    xTaskCreatePinnedToCore(loader_task, "tile_ld", 4096, NULL, 2, NULL, 0);
}

void loader_enqueue(int slot, int z, int x, int y, uint32_t gen)
{
    if (!s_req_q) return;
    load_req_t req = {.slot = slot, .z = z, .x = x, .y = y, .gen = gen};
    xQueueSend(s_req_q, &req, 0);
}

bool loader_poll_done(load_done_t *out)
{
    if (!s_done_q || !out) return false;
    return xQueueReceive(s_done_q, out, 0) == pdTRUE;
}

void loader_release(uint8_t *scratch)
{
    if (!scratch || !s_free_q) return;
    xQueueSend(s_free_q, &scratch, 0);
}
