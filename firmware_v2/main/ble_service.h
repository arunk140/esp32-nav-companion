#pragma once
// BLE contract — identical to firmware v1 (firmware/main/ble/ble_service.h)
// and mirrored by app/.../BleManager.kt. Do not change one side alone.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NAV_SVC_UUID_STR "e8dffd56-e8b9-4279-9bff-1e0d2220d776"
#define NAV_CHR_POSITION_UUID "26a57cd7-b97d-4414-8e56-d7a5fdb3d5d0"
#define NAV_CHR_CMD_UUID "b18e4af0-21d2-486f-ba3c-85dbedacdf2c"
#define NAV_CHR_STATUS_UUID "d7865290-9778-45bd-8faa-c3f5674b2457"

// NimBLE stores 128-bit UUIDs little-endian: the string's bytes reversed.
#define NAV_SVC_UUID_LE 0x76, 0xd7, 0x20, 0x22, 0x0d, 0x1e, 0xff, 0x9b, \
                        0x79, 0x42, 0xb9, 0xe8, 0x56, 0xfd, 0xdf, 0xe8
#define NAV_CHR_POSITION_UUID_LE 0xd0, 0xd5, 0xb3, 0xfd, 0xa5, 0xd7, 0x56, 0x8e, \
                                 0x14, 0x44, 0x7d, 0xb9, 0xd7, 0x7c, 0xa5, 0x26
#define NAV_CHR_CMD_UUID_LE 0x2c, 0xdf, 0xac, 0xed, 0xdb, 0x85, 0x3c, 0xba, \
                            0x6f, 0x48, 0xd2, 0x21, 0xf0, 0x4a, 0x8e, 0xb1
#define NAV_CHR_STATUS_UUID_LE 0x57, 0x24, 0x4b, 0x67, 0xf5, 0xc3, 0xaa, 0x8f, \
                               0xbd, 0x45, 0x78, 0x97, 0x90, 0x52, 0x86, 0xd7

// v2 media extension (added 2026-10-02). Phone-side mirror: BleManager.kt.
#define NAV_CHR_MEDIA_INFO_UUID "790d1d86-2191-404d-a0f5-0e960990e190"
#define NAV_CHR_MEDIA_CMD_UUID "f8a580f6-aa84-4898-ad67-a66c12262ac1"
#define NAV_CHR_MEDIA_INFO_UUID_LE 0x90, 0xe1, 0x90, 0x09, 0x96, 0x0e, 0xf5, 0xa0, \
                                   0x4d, 0x40, 0x91, 0x21, 0x86, 0x1d, 0x0d, 0x79
#define NAV_CHR_MEDIA_CMD_UUID_LE 0xc1, 0x2a, 0x26, 0x12, 0x6c, 0xa6, 0x67, 0xad, \
                                  0x98, 0x48, 0x84, 0xaa, 0xf6, 0x80, 0xa5, 0xf8

// MEDIA_INFO (phone -> device, write / write-no-rsp), little-endian:
//   0x01 STATE (13 B): u8 type, u8 flags, u8 volume 0-100, u32 position ms,
//                      u32 duration ms (0 = unknown), u16 art id (0 = none)
//   0x02 ART (11 B + data): u8 type, u16 art id, u32 offset, u32 total, data
//        Chunks of one 200x200 baseline JPEG (<= NAV_ART_MAX_BYTES), sent in
//        order. A new art id abandons any transfer in progress.
#define NAV_MEDIA_STATE 0x01
#define NAV_MEDIA_ART 0x02
#define NAV_MEDIA_STATE_LEN 13
#define NAV_MEDIA_ART_HDR 11
#define NAV_MEDIA_F_PLAYING 0x01
#define NAV_MEDIA_F_SESSION 0x02     // a player session exists
#define NAV_MEDIA_F_NO_ACCESS 0x04   // app lacks Notification access
#define NAV_ART_PX 200
#define NAV_ART_MAX_BYTES (48 * 1024)

// MEDIA_CMD (device -> phone, notify): u8 cmd [, u8 arg]
#define NAV_MCMD_PLAY_PAUSE 0x01
#define NAV_MCMD_NEXT 0x02
#define NAV_MCMD_PREV 0x03
#define NAV_MCMD_VOL_UP 0x04
#define NAV_MCMD_VOL_DOWN 0x05
#define NAV_MCMD_SET_VOLUME 0x06  // u8 0-100
#define NAV_MCMD_SYNC 0x07        // resend STATE + ART

// CMD (write, 1 opcode byte + args)
#define NAV_CMD_RECENTER 0x01
#define NAV_CMD_SET_ZOOM 0x02        // u8 zoom
#define NAV_CMD_PING 0x03
#define NAV_CMD_UNPAIR 0x04
#define NAV_CMD_SET_BRIGHTNESS 0x05  // u8 0-100 (5 % floor applied on device)
#define NAV_CMD_PAIR 0x06            // 6 ASCII digits = QR token

// POSITION (write / write-no-rsp), 18 bytes little-endian:
//   i32 lat 1e-7 deg, i32 lon 1e-7 deg, u16 speed cm/s, u16 cog 0.01 deg,
//   u8 sats, u8 fix, u32 phone timestamp ms
#define NAV_POSITION_LEN 18

// STATUS (read/notify): u16 battery mV (0 = USB), u8 link, u8 fps

typedef struct {
    int32_t lat_e7;
    int32_t lon_e7;
    uint16_t speed_cms;
    uint16_t cog_cdeg;
    uint8_t sats;
    uint8_t fix;
    uint32_t ts_ms;
} nav_position_t;

typedef struct {
    uint8_t flags;
    uint8_t volume;
    uint32_t position_ms;
    uint32_t duration_ms;
    uint16_t art_id;
} nav_media_state_t;

// All callbacks run in the NimBLE host task: copy and return. Never call
// LVGL or write NVS from them.
typedef struct {
    void (*on_position)(const nav_position_t *pos);
    void (*on_cmd)(uint8_t code, const uint8_t *args, size_t len);
    void (*on_link)(bool connected);
    void (*on_media_state)(const nav_media_state_t *st);
    // A whole ART write, header included (data[0] == NAV_MEDIA_ART).
    void (*on_media_art)(const uint8_t *data, size_t len);
} nav_ble_cbs_t;

void nav_ble_start(const nav_ble_cbs_t *cbs, const char *device_name);
bool nav_ble_connected(void);
void nav_ble_set_fps(uint8_t fps);

// Sends a MEDIA_CMD notification (any task; no-op unless the phone has
// subscribed).
void nav_ble_media_cmd(uint8_t cmd, uint8_t arg);
bool nav_ble_media_ready(void);

// Both are safe from any task (work is posted to the NimBLE host task).
// Drops the current link; bonds are kept.
void nav_ble_disconnect(void);
// Deletes the bond of the currently connected phone, then drops the link.
// Only for a phone-initiated unpair (the phone removes its side too):
// erasing a bond the phone still holds makes Android loop on "pair again".
void nav_ble_forget_current(void);
