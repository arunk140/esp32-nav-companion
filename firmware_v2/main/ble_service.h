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

// Turn-by-turn navigation, fed by OsmAnd's AIDL API on the phone. The phone
// owns routing and the instruction text; the device owns the map and the
// maneuver icons.
//   0x01 STATE (12 B incl. the type byte): u8 flags, u8 maneuver, u16 dist_m,
//        u8 after_maneuver, u16 after_dist_m, u16 remaining_m, u16 remaining_s
//        (metres / seconds, clamped to u16)
//   0x02 TEXT: u8 type, u8 len, street-name bytes (Latin-1; the device drops
//        anything it cannot draw, like the media title)
//   0x03 DEST (10 B): u8 type, u8 present, i32 lat_e7, i32 lon_e7. Sent only
//        when the destination changes; present=0 clears the marker.
// Maneuver codes are OUR enum (NAV_* below), deliberately not OsmAnd's
// TurnType numbering: that is an implementation detail of another project.
#define NAV_CHR_STATE_UUID "7c3d1f92-5a6b-4d18-9e2f-8b41c05d7a63"
#define NAV_CHR_STATE_UUID_LE 0x63, 0x7a, 0x5d, 0xc0, 0x41, 0x8b, 0x2f, 0x9e, \
                                       0x18, 0x4d, 0x6b, 0x5a, 0x92, 0x1f, 0x3d, 0x7c
#define NAV_STATE 0x01
#define NAV_TEXT 0x02
#define NAV_DEST 0x03
#define NAV_STATE_LEN 12
#define NAV_DEST_LEN 10
#define NAV_TEXT_MAX 48
#define NAV_F_ACTIVE 0x01   // navigation is running
#define NAV_F_NO_ACCESS 0x02 // app could not read OsmAnd
// remaining_m on the wire is in units of 10 m, which reaches 655 km. Without
// it the unit is metres and the field stops at 65.5 km (older phone apps).
#define NAV_F_REM_10M 0x04

// Route geometry for the map (added 2026-10-05). Valhalla returns the route
// shape (OsmAnd never did), so the device can draw the actual line. A route can
// be hundreds of points, so it arrives in chunks on the same characteristic:
//   [0] 0x04, [1] flags (bit0 START, bit1 END), [2] seq, [3] count,
//   [4..] count * (i32 lat_e7, i32 lon_e7), little-endian.
// A single START|END chunk with count=0 clears the route. Chunks are written
// with tag=null (never coalesced), unlike STATE.
#define NAV_ROUTE 0x04
#define NAV_ROUTE_HDR 4
#define NAV_ROUTE_CHUNK_MAX 31
#define NAV_ROUTE_F_START 0x01
#define NAV_ROUTE_F_END 0x02

// Posted speed limit and the units to show speeds in (added 2026-10-09):
//   [0] 0x05, [1] units (0 = km/h, 1 = mph), [2..3] u16 limit in those units,
//   0 = not known. Sent when either changes and once after connecting, so the
//   units arrive even where no limit is known.
#define NAV_LIMIT 0x05
#define NAV_LIMIT_LEN 4

// Nearby streets for the minimal display (added 2026-10-09): outlines of the
// named roads around the phone, so the route is not floating in empty black.
// Sent in ordered chunks on the nav characteristic, never coalesced:
//   [0] 0x06, [1] flags (bit0 START, bit1 END), [2] seq, [3] 0
//   START only: [4..7] i32 lat_e7, [8..11] i32 lon_e7 of the origin
//   then records, each whole within its chunk:
//     u8 n (2..NAV_ROADS_LINE_MAX), u8 class (0 minor, 1 major),
//     n * (i16 dlat, i16 dlon) in 1e-5 degrees from the origin
// A new set replaces the old one when its END arrives. START|END with no
// records clears them.
#define NAV_ROADS 0x06
#define NAV_ROADS_HDR 4
#define NAV_ROADS_F_START 0x01
#define NAV_ROADS_F_END 0x02
#define NAV_ROADS_LINE_MAX 60
#define NAV_ROADS_CHUNK_MAX 252

// The phone's battery and clock (added 2026-10-09). 12 bytes:
//   [0] 0x07, [1] battery percent (0xFF = unknown), [2] flags (bit0 charging),
//   [3] 0, [4..7] u32 Unix time in seconds, [8..9] i16 minutes east of UTC,
//   [10..11] 0. Sent after connecting and about once a minute.
#define NAV_PHONE 0x07
#define NAV_PHONE_LEN 12

#define NAV_NONE 0
#define NAV_CONTINUE 1
#define NAV_SLIGHT_LEFT 2
#define NAV_LEFT 3
#define NAV_SHARP_LEFT 4
#define NAV_SLIGHT_RIGHT 5
#define NAV_RIGHT 6
#define NAV_SHARP_RIGHT 7
#define NAV_KEEP_LEFT 8
#define NAV_KEEP_RIGHT 9
#define NAV_UTURN 10
#define NAV_ROUNDABOUT 11
#define NAV_OFF_ROUTE 12
#define NAV_ARRIVE 13
#define NAV_MANEUVER_MAX NAV_ARRIVE

// MEDIA_INFO (phone -> device, write / write-no-rsp), little-endian:
//   0x01 STATE (13 B): u8 type, u8 flags, u8 volume 0-100, u32 position ms,
//                      u32 duration ms (0 = unknown), u16 art id (0 = none)
//   0x02 ART (11 B + data): u8 type, u16 art id, u32 offset, u32 total, data
//        Chunks of one 200x200 baseline JPEG (<= NAV_ART_MAX_BYTES), sent in
//        order. A new art id abandons any transfer in progress.
//   0x03 TEXT (<= 160 B, only when the track changes): u8 type, title, 0x00,
//        artist, 0x00 — UTF-8, but the device only has a Latin font, so it
//        drops anything else rather than showing empty boxes.
#define NAV_MEDIA_STATE 0x01
#define NAV_MEDIA_ART 0x02
#define NAV_MEDIA_TEXT 0x03
#define NAV_MEDIA_TEXT_MAX 160
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

// Incoming-call screen (added 2026-10-03). Phone-side mirror: CallBridge.kt.
// Only ring/end are sent; the device decides what to show. Answer/reject go
// back on CALL_CMD.
//   0x01 RING: u8 type, name, 0x00, number, 0x00 — UTF-8. The phone sends this
//        once when a call starts ringing. Either field may be empty: no
//        contact match (name) or no READ_CALL_LOG (number). The device drops
//        non-Latin characters, same as media/nav text.
//   0x02 END: u8 type — the call is no longer ringing (answered elsewhere,
//        rejected, or missed). Hides the screen.
#define NAV_CHR_CALL_INFO_UUID "3f9a2b41-6c7d-4e58-9a1b-2d4e6f8a0c13"
#define NAV_CHR_CALL_CMD_UUID "8e1c7d35-2b4a-4f60-9c3e-5a7b1d2e4f68"
#define NAV_CHR_CALL_INFO_UUID_LE 0x13, 0x0c, 0x8a, 0x6f, 0x4e, 0x2d, 0x1b, 0x9a, \
                                  0x58, 0x4e, 0x7d, 0x6c, 0x41, 0x2b, 0x9a, 0x3f
#define NAV_CHR_CALL_CMD_UUID_LE 0x68, 0x4f, 0x2e, 0x1d, 0x7b, 0x5a, 0x3e, 0x9c, \
                                 0x60, 0x4f, 0x4a, 0x2b, 0x35, 0x7d, 0x1c, 0x8e
#define NAV_CALL_RING 0x01
#define NAV_CALL_END 0x02
#define NAV_CALL_TEXT_MAX 96

// CALL_CMD (device -> phone, notify): u8 cmd
#define NAV_CCMD_ANSWER 0x01
#define NAV_CCMD_REJECT 0x02

// CMD (write, 1 opcode byte + args)
#define NAV_CMD_RECENTER 0x01
#define NAV_CMD_SET_ZOOM 0x02        // u8 zoom
#define NAV_CMD_PING 0x03
#define NAV_CMD_UNPAIR 0x04
#define NAV_CMD_SET_BRIGHTNESS 0x05  // u8 0-100 (5 % floor applied on device)
#define NAV_CMD_PAIR 0x06            // 6 ASCII digits = QR token
#define NAV_CMD_SLEEP 0x07           // deep sleep now; touch is the only way back
#define NAV_CMD_SET_THEME 0x08       // u8: 0 = light map, 1 = dark map
#define NAV_CMD_SET_MINIMAL 0x09     // u8: 0 = map, 1 = minimal display (no tiles)

// POSITION (write / write-no-rsp), 18 bytes little-endian:
//   i32 lat 1e-7 deg, i32 lon 1e-7 deg, u16 speed cm/s, u16 cog 0.01 deg,
//   u8 sats, u8 fix, u32 phone timestamp ms
#define NAV_POSITION_LEN 18

// STATUS (read/notify): u16 battery mV (0 = USB), u8 link, u8 fps,
// u8 theme (0 light, 1 dark), u8 minimal display (0 map, 1 minimal; added
// 2026-10-09, absent on older firmware)

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

typedef struct {
    uint8_t flags;             // NAV_F_*
    uint8_t maneuver;          // NAV_*
    uint16_t dist_m;           // to the maneuver
    uint8_t after_maneuver;
    uint16_t after_dist_m;
    uint32_t remaining_m;      // metres, whatever unit it arrived in
    uint16_t remaining_s;
} nav_state_t;

// All callbacks run in the NimBLE host task: copy and return. Never call
// LVGL or write NVS from them.
typedef struct {
    void (*on_position)(const nav_position_t *pos);
    void (*on_cmd)(uint8_t code, const uint8_t *args, size_t len);
    void (*on_link)(bool connected);
    void (*on_media_state)(const nav_media_state_t *st);
    // A whole ART write, header included (data[0] == NAV_MEDIA_ART).
    void (*on_media_art)(const uint8_t *data, size_t len);
    // Title and artist of the current track, NUL-separated.
    void (*on_media_text)(const char *title, const char *artist);

    // Incoming call started ringing. name/number may be empty; both may
    // contain non-Latin characters (the GUI drops what it cannot draw).
    void (*on_call_ring)(const char *name, const char *number);
    // Ringing stopped (answered, rejected or missed): hide the call screen.
    void (*on_call_end)(void);

    // Navigation: NAV_STATE decoded (st may be NULL when only TEXT arrived,
    // because the two are separate writes); street may be empty.
    void (*on_nav_state)(const nav_state_t *st, const char *street);
    // Destination for the map marker. present=false means "no destination"
    // (navigation ended): clear it.
    void (*on_nav_dest)(bool present, int32_t lat_e7, int32_t lon_e7);

    // One NAV_ROADS write, header included (data[0] == NAV_ROADS). Runs in the
    // NimBLE host task: copy and return.
    void (*on_roads_chunk)(const uint8_t *data, size_t len);

    // Phone battery (-1 unknown) and local time as seconds since the epoch.
    void (*on_phone_info)(int battery_pct, bool charging, uint32_t local_epoch_s);

    // Speed limit in the phone's units; limit 0 = unknown. mph is false for km/h.
    void (*on_nav_limit)(bool mph, uint16_t limit);

    // Route geometry chunk. Runs in the NimBLE host task: copy and return.
    // count <= NAV_ROUTE_CHUNK_MAX; lat_e7/lon_e7 point to count valid entries.
    void (*on_route_chunk)(uint8_t flags, uint8_t seq, uint8_t count,
                           const int32_t *lat_e7, const int32_t *lon_e7);
} nav_ble_cbs_t;

void nav_ble_start(const nav_ble_cbs_t *cbs, const char *device_name);
bool nav_ble_connected(void);
void nav_ble_set_fps(uint8_t fps);
void nav_ble_set_theme(uint8_t dark);  // reported back in STATUS
void nav_ble_set_minimal(uint8_t on);  // reported back in STATUS

// Sends a MEDIA_CMD notification (any task; no-op unless the phone has
// subscribed).
void nav_ble_media_cmd(uint8_t cmd, uint8_t arg);
bool nav_ble_media_ready(void);

// Sends a CALL_CMD notification (any task; no-op unless the phone has
// subscribed).
void nav_ble_call_cmd(uint8_t cmd);
bool nav_ble_call_ready(void);

// Both are safe from any task (work is posted to the NimBLE host task).
// Drops the current link; bonds are kept.
void nav_ble_disconnect(void);
// Deletes the bond of the currently connected phone, then drops the link.
// Only for a phone-initiated unpair (the phone removes its side too):
// erasing a bond the phone still holds makes Android loop on "pair again".
void nav_ble_forget_current(void);
