#include "ble/ble_service.h"
#include "settings.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_sm.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <string.h>

// NVS-backed bond store, provided by the NimBLE config store.
void ble_store_config_init(void);

static const char *TAG = "ble";

static nav_ble_cbs_t s_cbs;
static bool s_connected;
static uint8_t s_own_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

// characteristic handles for notify
static uint16_t s_status_val_h;

static int gap_event(struct ble_gap_event *ev, void *arg);
static void advertise(void);

static int chr_write_pos(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len != NAV_POSITION_LEN) {
        ESP_LOGW(TAG, "bad position len %d", len);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    uint8_t b[NAV_POSITION_LEN];
    ble_hs_mbuf_to_flat(ctxt->om, b, sizeof(b), NULL);
    int32_t lat = (int32_t)(b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24));
    int32_t lon = (int32_t)(b[4] | (b[5] << 8) | (b[6] << 16) | (b[7] << 24));
    uint16_t spd = b[8] | (b[9] << 8);
    uint16_t cog = b[10] | (b[11] << 8);
    if (s_cbs.on_position) s_cbs.on_position(lat, lon, spd, cog, b[12], b[13],
                                             b[14] | (b[15] << 8) | (b[16] << 16) | (b[17] << 24));
    return 0;
}

static int chr_write_cmd(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len < 1 || len > 8) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint8_t b[8];
    ble_hs_mbuf_to_flat(ctxt->om, b, sizeof(b), NULL);
    ESP_LOGI(TAG, "cmd 0x%02x len %d", b[0], len);
    if (s_cbs.on_cmd) s_cbs.on_cmd(b[0], b + 1, len - 1);
    return 0;
}

static int chr_read_status(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    // [battery_mV u16 (0 = USB/no battery, Phase 1)] [link u8] [fps u8]
    uint8_t s[4] = {0, 0, s_connected ? 1 : 0, 0};
    return os_mbuf_append(ctxt->om, s, sizeof(s));
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID128_DECLARE(0x76, 0xd7, 0x20, 0x22, 0x0d, 0x1e, 0xff, 0x9b,
                                    0x79, 0x42, 0xb9, 0xe8, 0x56, 0xfd, 0xdf, 0xe8),
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = BLE_UUID128_DECLARE(0xd0, 0xd5, 0xb3, 0xfd, 0xa5, 0xd7, 0x56, 0x8e,
                                            0x14, 0x44, 0x7d, 0xb9, 0xd7, 0x7c, 0xa5, 0x26),
                .access_cb = chr_write_pos,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = BLE_UUID128_DECLARE(0x2c, 0xdf, 0xac, 0xed, 0xdb, 0x85, 0x3c, 0xba,
                                            0x6f, 0x48, 0xd2, 0x21, 0xf0, 0x4a, 0x8e, 0xb1),
                .access_cb = chr_write_cmd,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = BLE_UUID128_DECLARE(0x57, 0x24, 0x4b, 0x67, 0xf5, 0xc3, 0xaa, 0x8f,
                                            0xbd, 0x45, 0x78, 0x97, 0x90, 0x52, 0x86, 0xd7),
                .access_cb = chr_read_status,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_status_val_h,
            },
            {0},
        },
    },
    {0},
};

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn_handle = ev->connect.conn_handle;
            s_connected = true;
            ESP_LOGI(TAG, "connected");
            if (s_cbs.on_connect) s_cbs.on_connect();
        } else {
            advertise();  // failed; resume advertising
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_connected = false;
        ESP_LOGI(TAG, "disconnected reason %d", ev->disconnect.reason);
        if (s_cbs.on_disconnect) s_cbs.on_disconnect();
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();  // keep advertising
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        break;
    default:
        break;
    }
    return 0;
}

static void advertise(void)
{
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (uint8_t *)settings_get_device_name();
    f.name_len = strlen(settings_get_device_name());
    f.name_is_complete = 1;
    // Service UUID in advertisement for filtered scanning.
    // Bytes are reverse-string-order per ble_uuid_to_str (little-endian).
    static const ble_uuid128_t svc = BLE_UUID128_INIT(0x76, 0xd7, 0x20, 0x22, 0x0d, 0x1e,
                                                      0xff, 0x9b, 0x79, 0x42, 0xb9, 0xe8,
                                                      0x56, 0xfd, 0xdf, 0xe8);
    // f carries flags + name only (a 128-bit UUID would overflow the packet).
    ble_gap_adv_set_fields(&f);
    // 128-bit UUIDs go in the scan response (adv packet too small with name).
    struct ble_hs_adv_fields rsp = {0};
    rsp.uuids128 = &svc;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params p = {0};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = 0x20;
    p.itvl_max = 0x30;
    ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
}

static void on_sync(void)
{
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    advertise();
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void nav_ble_start(const nav_ble_cbs_t *cbs)
{
    if (cbs) s_cbs = *cbs;
    nimble_port_init();
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    // Just Works bonding (NoIO display), keys persisted to NVS.
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(gatt_svcs);
    ble_gatts_add_svcs(gatt_svcs);
    ble_svc_gap_device_name_set(settings_get_device_name());
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
}

void nav_ble_set_bonded(bool bonded)
{
    settings_set_paired(bonded);
}

bool nav_ble_connected(void)
{
    return s_connected;
}
