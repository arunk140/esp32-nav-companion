#include "ble_service.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <string.h>

static const char *TAG = "ble";

void ble_store_config_init(void);

static nav_ble_cbs_t s_cbs;
static char s_name[24];
static uint8_t s_own_addr_type;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile uint8_t s_fps;
static uint16_t s_status_h;
static struct ble_npl_event s_forget_ev, s_disc_ev, s_mcmd_ev;
static uint16_t s_media_cmd_h;
static volatile bool s_media_sub;
static QueueHandle_t s_mcmd_q;  // GUI -> host task, 2-byte commands

static void advertise(void);

static inline uint16_t le16(const uint8_t *b)
{
    return (uint16_t)(b[0] | (b[1] << 8));
}

static inline uint32_t le32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static int chr_position(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    if (OS_MBUF_PKTLEN(ctxt->om) != NAV_POSITION_LEN) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint8_t b[NAV_POSITION_LEN];
    if (ble_hs_mbuf_to_flat(ctxt->om, b, sizeof(b), NULL) != 0) return BLE_ATT_ERR_UNLIKELY;
    nav_position_t p = {
        .lat_e7 = (int32_t)le32(b),
        .lon_e7 = (int32_t)le32(b + 4),
        .speed_cms = le16(b + 8),
        .cog_cdeg = le16(b + 10),
        .sats = b[12],
        .fix = b[13],
        .ts_ms = le32(b + 14),
    };
    if (s_cbs.on_position) s_cbs.on_position(&p);
    return 0;
}

static int chr_cmd(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len < 1 || len > 8) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint8_t b[8];
    if (ble_hs_mbuf_to_flat(ctxt->om, b, sizeof(b), NULL) != 0) return BLE_ATT_ERR_UNLIKELY;
    if (s_cbs.on_cmd) s_cbs.on_cmd(b[0], b + 1, len - 1);
    return 0;
}

static int chr_status(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t s[4] = {0, 0, s_conn != BLE_HS_CONN_HANDLE_NONE ? 1 : 0, s_fps};
    return os_mbuf_append(ctxt->om, s, sizeof(s)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int chr_media_info(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    uint8_t b[256];
    if (len < 1 || len > sizeof(b)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (ble_hs_mbuf_to_flat(ctxt->om, b, sizeof(b), NULL) != 0) return BLE_ATT_ERR_UNLIKELY;
    if (b[0] == NAV_MEDIA_STATE) {
        if (len < NAV_MEDIA_STATE_LEN) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        nav_media_state_t st = {
            .flags = b[1],
            .volume = b[2],
            .position_ms = le32(b + 3),
            .duration_ms = le32(b + 7),
            .art_id = le16(b + 11),
        };
        if (s_cbs.on_media_state) s_cbs.on_media_state(&st);
    } else if (b[0] == NAV_MEDIA_ART) {
        if (len < NAV_MEDIA_ART_HDR) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (s_cbs.on_media_art) s_cbs.on_media_art(b, len);
    }
    return 0;
}

static int chr_media_cmd(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t z = 0;
    return os_mbuf_append(ctxt->om, &z, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(NAV_SVC_UUID_LE);
static const ble_uuid128_t s_pos_uuid = BLE_UUID128_INIT(NAV_CHR_POSITION_UUID_LE);
static const ble_uuid128_t s_cmd_uuid = BLE_UUID128_INIT(NAV_CHR_CMD_UUID_LE);
static const ble_uuid128_t s_status_uuid = BLE_UUID128_INIT(NAV_CHR_STATUS_UUID_LE);
static const ble_uuid128_t s_media_info_uuid = BLE_UUID128_INIT(NAV_CHR_MEDIA_INFO_UUID_LE);
static const ble_uuid128_t s_media_cmd_uuid = BLE_UUID128_INIT(NAV_CHR_MEDIA_CMD_UUID_LE);

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &s_pos_uuid.u,
                .access_cb = chr_position,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &s_cmd_uuid.u,
                .access_cb = chr_cmd,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &s_status_uuid.u,
                .access_cb = chr_status,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_status_h,
            },
            {
                .uuid = &s_media_info_uuid.u,
                .access_cb = chr_media_info,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &s_media_cmd_uuid.u,
                .access_cb = chr_media_cmd,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_media_cmd_h,
            },
            {0},
        },
    },
    {0},
};

#if CONFIG_NAV_DEBUG_BLE
#define DBG(...) ESP_LOGW(TAG, __VA_ARGS__)
#else
#define DBG(...)
#endif

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(ev->enc_change.conn_handle, &d) == 0) {
            DBG("enc_change status=%d enc=%d auth=%d bonded=%d", ev->enc_change.status,
                d.sec_state.encrypted, d.sec_state.authenticated, d.sec_state.bonded);
        }
        return 0;
    }
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        DBG("passkey action %d", ev->passkey.params.action);
        return 0;
    case BLE_GAP_EVENT_IDENTITY_RESOLVED:
        DBG("identity resolved");
        return 0;
    case BLE_GAP_EVENT_CONNECT:
        DBG("connect status=%d", ev->connect.status);
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            if (s_cbs.on_link) s_cbs.on_link(true);
        } else {
            advertise();
        }
        return 0;
    case BLE_GAP_EVENT_NOTIFY_TX:
        DBG("notify_tx handle=%d status=%d indication=%d", ev->notify_tx.attr_handle,
            ev->notify_tx.status, ev->notify_tx.indication);
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        DBG("subscribe handle=%d reason=%d notify=%d indicate=%d", ev->subscribe.attr_handle,
            ev->subscribe.reason, ev->subscribe.cur_notify, ev->subscribe.cur_indicate);
        if (ev->subscribe.attr_handle == s_media_cmd_h) {
            s_media_sub = ev->subscribe.cur_notify;
            DBG("media cmd subscribe=%d", s_media_sub);
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        DBG("disconnect reason=0x%x", ev->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_media_sub = false;
        if (s_cbs.on_link) s_cbs.on_link(false);
        advertise();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_conn == BLE_HS_CONN_HANDLE_NONE) advertise();
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // The phone lost its bond but we still have one: drop ours and let
        // pairing proceed, instead of failing silently forever.
        DBG("repeat pairing");
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
        return 0;
    }
}

static void advertise(void)
{
    if (ble_gap_adv_active()) return;

    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = &s_svc_uuid;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc) ESP_LOGW(TAG, "adv fields rc=%d", rc);

    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (const uint8_t *)s_name;
    rsp.name_len = strlen(s_name);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc) ESP_LOGW(TAG, "rsp fields rc=%d", rc);

    struct ble_gap_adv_params p = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = BLE_GAP_ADV_ITVL_MS(50),
        .itvl_max = BLE_GAP_ADV_ITVL_MS(100),
    };
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc) ESP_LOGW(TAG, "adv start rc=%d", rc);
}

// ---- bond store hygiene -------------------------------------------------
// NimBLE writes an RPA record (phone's private address -> identity) on every
// pairing, in a table only MAX_BONDS deep. ble_store_clear() never removes
// them (it matches on an all-zero address), so they pile up. When the table
// is full, the stock ble_store_util_status_rr() "makes room" by unpairing
// the oldest peer: that is the phone that just bonded. It loses its fresh
// keys and the link drops, and Android loops on "pair again".

#define MAX_RPA CONFIG_BT_NIMBLE_MAX_BONDS

// The table itself (store/config). Read directly: ble_store_iterate() has the
// same zero-address lookup bug and only ever returns records whose private
// address is all-zero, i.e. it cannot see the records that pile up.
extern struct ble_store_value_rpa_rec ble_store_config_rpa_recs[];
extern int ble_store_config_num_rpa_recs;

static int rpa_snapshot(struct ble_store_value_rpa_rec *out)
{
    int n = ble_store_config_num_rpa_recs;
    if (n > MAX_RPA) n = MAX_RPA;
    memcpy(out, ble_store_config_rpa_recs, n * sizeof(*out));
    return n;
}

static bool is_bonded(const ble_addr_t *a)
{
    ble_addr_t peers[MAX_RPA];
    int n = 0;
    if (ble_store_util_bonded_peers(peers, &n, MAX_RPA) != 0) return false;
    for (int i = 0; i < n; i++) {
        if (ble_addr_cmp(&peers[i], a) == 0) return true;
    }
    return false;
}

static void delete_rpa_rec(const struct ble_store_value_rpa_rec *r)
{
    struct ble_store_key_rpa_rec key = {.peer_rpa_addr = r->peer_rpa_addr};
    ble_store_delete_rpa_rec(&key);
}

// Deletes RPA records that belong to no bond. If none are orphaned and
// `incoming` is given (overflow), deletes the oldest record of another peer.
// Returns the number deleted.
static int purge_rpa_recs(const ble_addr_t *incoming)
{
    struct ble_store_value_rpa_rec recs[MAX_RPA];
    int n = rpa_snapshot(recs);
    int deleted = 0;
    for (int i = 0; i < n; i++) {
        if (!is_bonded(&recs[i].peer_addr)) {
            delete_rpa_rec(&recs[i]);
            deleted++;
        }
    }
    if (!deleted && incoming) {
        for (int i = 0; i < n; i++) {
            if (ble_addr_cmp(&recs[i].peer_addr, incoming) != 0) {
                delete_rpa_rec(&recs[i]);
                deleted = 1;
                break;
            }
        }
    }
    if (deleted) DBG("purged %d stale RPA records", deleted);
    return deleted;
}

static int store_status_cb(struct ble_store_status_event *ev, void *arg)
{
    if (ev->event_code == BLE_STORE_EVENT_OVERFLOW &&
        ev->overflow.obj_type == BLE_STORE_OBJ_TYPE_PEER_ADDR) {
        // Never unpair a phone over a full RPA table: drop a record instead.
        const ble_addr_t *in = &ev->overflow.value->rpa_rec.peer_addr;
        return purge_rpa_recs(in) ? 0 : BLE_HS_ESTORE_CAP;
    }
    return ble_store_util_status_rr(ev, arg);
}

static void on_sync(void)
{
#if CONFIG_NAV_DEBUG_BLE
    {
        struct ble_store_value_rpa_rec recs[MAX_RPA];
        int nr = rpa_snapshot(recs);
        ble_addr_t peers[MAX_RPA];
        int n = 0;
        ble_store_util_bonded_peers(peers, &n, MAX_RPA);
        DBG("store: %d bonds, %d RPA records", n, nr);
        for (int i = 0; i < nr; i++) {
            const uint8_t *a = recs[i].peer_addr.val, *r = recs[i].peer_rpa_addr.val;
            DBG("  rpa rec id %02x:%02x:%02x:%02x:%02x:%02x rpa %02x:%02x:%02x:%02x:%02x:%02x bonded=%d",
                a[5], a[4], a[3], a[2], a[1], a[0], r[5], r[4], r[3], r[2], r[1], r[0],
                is_bonded(&recs[i].peer_addr));
        }
    }
#endif
    purge_rpa_recs(NULL);
    // Bonded phones cache our GATT table and won't see characteristics added
    // by a firmware update. Service Changed (whole range) makes each of them
    // re-discover once; NimBLE queues it for phones that are offline now and
    // delivers it when they reconnect. Cheap, so do it every boot.
    ble_svc_gatt_changed(0x0001, 0xFFFF);
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason %d", reason);
}

static void forget_ev_cb(struct ble_npl_event *ev)
{
    (void)ev;
    uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE) return;
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(conn, &d) == 0) {
        DBG("forget peer bond");
        ble_store_util_delete_peer(&d.peer_id_addr);
    }
    ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
}

static void disc_ev_cb(struct ble_npl_event *ev)
{
    (void)ev;
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
}

static void mcmd_ev_cb(struct ble_npl_event *ev)
{
    (void)ev;
    uint8_t c[2];
    while (xQueueReceive(s_mcmd_q, c, 0) == pdTRUE) {
        if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_media_sub) continue;
        uint8_t n = c[0] == NAV_MCMD_SET_VOLUME ? 2 : 1;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(c, n);
        if (om) ble_gatts_notify_custom(s_conn, s_media_cmd_h, om);
    }
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void nav_ble_start(const nav_ble_cbs_t *cbs, const char *device_name)
{
    s_cbs = *cbs;
    strncpy(s_name, device_name, sizeof(s_name) - 1);

    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble init failed");
        return;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = store_status_cb;
    // Just Works + LE Secure Connections, bonded. Exchange identity keys so
    // the phone's private (rotating) address resolves on reconnect.
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(s_svcs);
    ble_gatts_add_svcs(s_svcs);
    ble_svc_gap_device_name_set(s_name);
    ble_store_config_init();

    ble_npl_event_init(&s_forget_ev, forget_ev_cb, NULL);
    ble_npl_event_init(&s_disc_ev, disc_ev_cb, NULL);
    ble_npl_event_init(&s_mcmd_ev, mcmd_ev_cb, NULL);
    s_mcmd_q = xQueueCreate(8, 2);
    nimble_port_freertos_init(host_task);
}

bool nav_ble_connected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE;
}

void nav_ble_set_fps(uint8_t fps)
{
    s_fps = fps;
}

void nav_ble_forget_current(void)
{
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_forget_ev);
}

void nav_ble_disconnect(void)
{
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_disc_ev);
}

void nav_ble_media_cmd(uint8_t cmd, uint8_t arg)
{
    uint8_t c[2] = {cmd, arg};
    if (!s_mcmd_q || xQueueSend(s_mcmd_q, c, 0) != pdTRUE) return;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_mcmd_ev);
}

bool nav_ble_media_ready(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE && s_media_sub;
}
