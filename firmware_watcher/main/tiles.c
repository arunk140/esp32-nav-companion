#include "tiles.h"
#include "sdcard.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "tiles";

#define TILE_DIR SD_MOUNT "/street_map"
#define TILE_BYTES (TILE_PX * TILE_PX * 2)
#define TILE_HDR 12
#define NSLOTS CONFIG_NAV_TILE_CACHE_SLOTS
// SD reads go into internal DMA-capable RAM: reading straight into PSRAM
// makes the SDMMC driver fall back to one 512 B sector per transaction.
#define CHUNK (32 * 1024)

typedef enum { S_FREE, S_LOADING, S_READY } slot_state_t;

// Negative cache: tiles known to be absent from the card (no pixel buffer).
#define NMISS 128

typedef struct {
    slot_state_t state;
    int8_t z;
    int32_t x, y;
    uint32_t last_used;
    bool dark;  // theme the pixels were written with
    uint8_t *px;
    lv_image_dsc_t dsc;
} slot_t;

typedef struct {
    int16_t slot;
    int8_t z;
    int32_t x, y;
    uint8_t *px;
} req_t;

typedef struct {
    int16_t slot;
    bool ok;
    bool skipped;
    bool dark;  // theme the pixels were written with
    uint32_t ms;
} done_t;

static slot_t s_slots[NSLOTS];
static QueueHandle_t s_req_q, s_done_q;
static uint8_t *s_chunk;
static volatile int s_active_zoom;
static volatile bool s_dark;
static uint32_t s_frame = 1;
static int s_zmin, s_zmax;
static int s_loading;
static uint32_t s_loads, s_load_ms_sum, s_load_ms_max, s_missing;
static struct { int8_t z; int32_t x, y; } s_miss[NMISS];
static int s_miss_next;

static bool is_missing(int z, int x, int y)
{
    for (int i = 0; i < NMISS; i++) {
        if (s_miss[i].z == z && s_miss[i].x == x && s_miss[i].y == y) return true;
    }
    return false;
}

static void add_missing(int z, int x, int y)
{
    s_miss[s_miss_next].z = z;
    s_miss[s_miss_next].x = x;
    s_miss[s_miss_next].y = y;
    s_miss_next = (s_miss_next + 1) % NMISS;
}

static bool read_tile(int z, int x, int y, uint8_t *dst)
{
    char path[64];
    snprintf(path, sizeof(path), TILE_DIR "/%d/%d/%d.bin", z, x, y);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    size_t total = TILE_HDR + TILE_BYTES;
    size_t got = 0;
    bool ok = true;
    while (got < total) {
        size_t want = total - got > CHUNK ? CHUNK : total - got;
        ssize_t n = read(fd, s_chunk, want);
        if (n <= 0) {
            ok = false;
            break;
        }
        const uint8_t *src = s_chunk;
        size_t len = (size_t)n;
        if (got == 0) {
            // LVGL v9 header: magic 0x19, cf 0x12 (RGB565), w, h = 256.
            if (len < TILE_HDR || src[0] != 0x19 || src[1] != 0x12 ||
                (src[4] | (src[5] << 8)) != TILE_PX || (src[6] | (src[7] << 8)) != TILE_PX) {
                ok = false;
                break;
            }
            memcpy(dst, src + TILE_HDR, len - TILE_HDR);
        } else {
            memcpy(dst + got - TILE_HDR, src, len);
        }
        got += len;
    }
    close(fd);
    return ok;
}

// Hue-preserving dark map: lightness flips, and each channel keeps 60% of
// its offset from the old lightness, so roads and water stay recognisable
// instead of turning into a photo negative. Channel values are expanded by
// bit replication (no division); 0.6 is one multiply.
#define CHROMA_SHIFT 11
#define CHROMA_MUL 1233  // 0.6016

static inline uint8_t clamp8(int32_t v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v);
}

static void apply_dark(uint8_t *px)
{
    uint16_t *p = (uint16_t *)px;
    for (int i = 0; i < TILE_PX * TILE_PX; i++) {
        uint32_t c = p[i];
        uint32_t r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
        uint32_t r = (r5 << 3) | (r5 >> 2);
        uint32_t g = (g6 << 2) | (g6 >> 4);
        uint32_t b = (b5 << 3) | (b5 >> 2);
        uint32_t l = ((r + g + b) * 85) >> 8;  // mean, 0..255
        uint32_t inv = 255 - l;
        int32_t r2 = (int32_t)inv + ((((int32_t)r - (int32_t)l) * CHROMA_MUL) >> CHROMA_SHIFT);
        int32_t g2 = (int32_t)inv + ((((int32_t)g - (int32_t)l) * CHROMA_MUL) >> CHROMA_SHIFT);
        int32_t b2 = (int32_t)inv + ((((int32_t)b - (int32_t)l) * CHROMA_MUL) >> CHROMA_SHIFT);
        p[i] = ((uint32_t)clamp8(r2) >> 3) << 11 | ((uint32_t)clamp8(g2) >> 2) << 5 |
               (clamp8(b2) >> 3);
    }
}

static void loader_task(void *arg)
{
    (void)arg;
    req_t r;
    for (;;) {
        if (xQueueReceive(s_req_q, &r, portMAX_DELAY) != pdTRUE) continue;
        done_t d = {.slot = r.slot};
        if (r.z != s_active_zoom) {
            d.skipped = true;
        } else {
            int64_t t0 = esp_timer_get_time();
            d.ok = read_tile(r.z, r.x, r.y, r.px);
            // Theme is sampled here, not at enqueue time: a switch that
            // lands while a tile is queued must still apply to it.
            d.dark = s_dark;
            if (d.ok && d.dark) apply_dark(r.px);
            d.ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        }
        // Never blocks: at most NSLOTS requests are outstanding.
        xQueueSend(s_done_q, &d, portMAX_DELAY);
    }
}

static void scan_zooms(void)
{
    s_zmin = s_zmax = 0;
    if (!sdcard_ok()) return;
    for (int z = 1; z <= 20; z++) {
        char p[40];
        struct stat st;
        snprintf(p, sizeof(p), TILE_DIR "/%d", z);
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (!s_zmin) s_zmin = z;
            s_zmax = z;
        }
    }
    if (!s_zmin) ESP_LOGE(TAG, "no tiles under " TILE_DIR);
}

void tiles_init(void)
{
#if CONFIG_NAV_DARK_MAP
    s_dark = true;
#endif
    scan_zooms();
    for (int i = 0; i < NMISS; i++) s_miss[i].z = -1;
    s_chunk = heap_caps_malloc(CHUNK, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_req_q = xQueueCreate(NSLOTS, sizeof(req_t));
    s_done_q = xQueueCreate(NSLOTS, sizeof(done_t));
    for (int i = 0; i < NSLOTS; i++) {
        slot_t *s = &s_slots[i];
        s->px = heap_caps_aligned_alloc(64, TILE_BYTES, MALLOC_CAP_SPIRAM);
        s->state = S_FREE;
        s->z = -1;
        if (!s->px) {
            ESP_LOGE(TAG, "tile cache OOM at slot %d", i);
            continue;
        }
        s->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        s->dsc.header.w = TILE_PX;
        s->dsc.header.h = TILE_PX;
        s->dsc.header.stride = TILE_PX * 2;
        s->dsc.data_size = TILE_BYTES;
        s->dsc.data = s->px;
    }
    if (s_chunk && sdcard_ok()) {
        xTaskCreatePinnedToCore(loader_task, "tile_ld", 4096, NULL, 3, NULL, 0);
    }
}

void tiles_set_dark(bool dark)
{
    if (dark == s_dark) return;
    s_dark = dark;
    for (int i = 0; i < NSLOTS; i++) {
        slot_t *s = &s_slots[i];
        // LOADING slots are left alone: the loader discards them if the
        // theme moved on while it was reading.
        if (s->state == S_READY || s->state == S_FREE) {
            s->state = S_FREE;
            s->z = -1;
        }
    }
}

bool tiles_dark(void)
{
    return s_dark;
}

int tiles_zoom_min(void)
{
    return s_zmin;
}

int tiles_zoom_max(void)
{
    return s_zmax;
}

void tiles_begin_frame(void)
{
    s_frame++;
}

void tiles_set_active_zoom(int z)
{
    s_active_zoom = z;
}

static slot_t *find(int z, int x, int y)
{
    for (int i = 0; i < NSLOTS; i++) {
        slot_t *s = &s_slots[i];
        if (s->z == z && s->x == x && s->y == y && s->state != S_FREE) return s;
    }
    return NULL;
}

// Least-recently-used slot that is neither loading nor pinned this frame.
static slot_t *evict(void)
{
    slot_t *best = NULL;
    for (int i = 0; i < NSLOTS; i++) {
        slot_t *s = &s_slots[i];
        if (!s->px || s->state == S_LOADING || s->last_used >= s_frame) continue;
        if (s->state == S_FREE) return s;
        if (!best || s->last_used < best->last_used) best = s;
    }
    return best;
}

static bool request(slot_t *s, int z, int x, int y, bool urgent)
{
    req_t r = {.slot = (int16_t)(s - s_slots), .z = z, .x = x, .y = y, .px = s->px};
    BaseType_t ok = urgent ? xQueueSendToFront(s_req_q, &r, 0) : xQueueSendToBack(s_req_q, &r, 0);
    if (ok != pdTRUE) return false;
    s->state = S_LOADING;
    s->z = z;
    s->x = x;
    s->y = y;
    s_loading++;
    return true;
}

static bool valid(int z, int x, int y)
{
    if (!s_zmin || z < s_zmin || z > s_zmax) return false;
    int32_t n = 1 << z;
    return x >= 0 && y >= 0 && x < n && y < n;
}

const lv_image_dsc_t *tiles_get(int z, int x, int y, bool urgent, tile_state_t *state)
{
    if (!valid(z, x, y) || !s_chunk) {
        *state = TILE_MISSING;
        return NULL;
    }
    slot_t *s = find(z, x, y);
    if (s && s->state == S_READY && s->dark != s_dark) {
        s->state = S_FREE;  // wrong theme: drop it, it will be re-read
        s->z = -1;
        s = NULL;
    }
    if (s) {
        if (urgent) s->last_used = s_frame;
        if (s->state == S_READY) {
            *state = TILE_READY;
            return &s->dsc;
        }
        *state = TILE_PENDING;
        return NULL;
    }
    if (is_missing(z, x, y)) {
        *state = TILE_MISSING;
        return NULL;
    }
    *state = TILE_PENDING;
    s = evict();
    if (!s) return NULL;  // everything pinned or loading; retried next frame
    s->state = S_FREE;
    s->z = -1;
    if (!request(s, z, x, y, urgent)) return NULL;
    if (urgent) s->last_used = s_frame;
    return NULL;
}

void tiles_prefetch(int z, int x, int y)
{
    if (!valid(z, x, y) || !s_chunk) return;
    slot_t *s = find(z, x, y);
    if (s) {
        // Keep it warmer than stale tiles, but below the pinned set.
        if (s->last_used < s_frame - 1) s->last_used = s_frame - 1;
        return;
    }
    if (is_missing(z, x, y)) return;
    // Leave headroom for urgent loads.
    if (s_loading >= NSLOTS / 3) return;
    s = evict();
    if (!s || s->last_used >= s_frame - 1) return;  // never evict the current neighbourhood
    s->state = S_FREE;
    s->z = -1;
    if (request(s, z, x, y, false)) s->last_used = s_frame - 1;
}

bool tiles_poll(void)
{
    bool changed = false;
    done_t d;
    while (s_done_q && xQueueReceive(s_done_q, &d, 0) == pdTRUE) {
        slot_t *s = &s_slots[d.slot];
        s_loading--;
        if (d.skipped) {
            s->state = S_FREE;
            s->z = -1;
            continue;
        }
        if (d.ok && d.dark != s_dark) {
            // Switched mid-read: throw the result away rather than showing a
            // tile in the old theme.
            s->state = S_FREE;
            s->z = -1;
        } else if (d.ok) {
            s->state = S_READY;
            s->dark = d.dark;
            s_loads++;
            s_load_ms_sum += d.ms;
            if (d.ms > s_load_ms_max) s_load_ms_max = d.ms;
        } else {
            add_missing(s->z, s->x, s->y);
            s->state = S_FREE;
            s->z = -1;
            s_missing++;
        }
        changed = true;
    }
    return changed;
}

void tiles_get_stats(tiles_stats_t *out)
{
    out->loads = s_loads;
    out->avg_load_ms = s_loads ? s_load_ms_sum / s_loads : 0;
    out->max_load_ms = s_load_ms_max;
    out->missing = s_missing;
    out->pending = s_loading;
    s_load_ms_max = 0;
}
