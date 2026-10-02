#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// BLE contract, Phase 1. Generated 2026-09-26, do not change (QR + app mirror them).
#define NAV_SVC_UUID_STR      "e8dffd56-e8b9-4279-9bff-1e0d2220d776"
#define NAV_CHR_POSITION_UUID "26a57cd7-b97d-4414-8e56-d7a5fdb3d5d0"
#define NAV_CHR_CMD_UUID      "b18e4af0-21d2-486f-ba3c-85dbedacdf2c"
#define NAV_CHR_STATUS_UUID   "d7865290-9778-45bd-8faa-c3f5674b2457"

// CMD codes (see docs/phase-1-plan.md)
#define NAV_CMD_RECENTER       0x01
#define NAV_CMD_SET_ZOOM       0x02
#define NAV_CMD_PING           0x03
#define NAV_CMD_UNPAIR         0x04
#define NAV_CMD_SET_BRIGHTNESS 0x05  // u8 0-100, 5% floor, NVS persisted
#define NAV_CMD_PAIR           0x06  // 6-byte ASCII token; on match -> bonded

// POSITION payload: 18 bytes, little-endian
//   int32 lat 1e-7 deg, int32 lon, u16 speed cm/s, u16 cog 0.01deg,
//   u8 sats, u8 fix, u32 phone_timestamp_ms
#define NAV_POSITION_LEN 18

// Callbacks into the app (implemented in main.c / map glue).
typedef struct {
    void (*on_position)(int32_t lat_e7, int32_t lon_e7, uint16_t speed_cms,
                        uint16_t cog_cdeg, uint8_t sats, uint8_t fix,
                        uint32_t ts_ms);
    void (*on_cmd)(uint8_t code, const uint8_t *args, size_t len);
    void (*on_connect)(void);
    void (*on_disconnect)(void);
} nav_ble_cbs_t;

// Starts NimBLE, advertising + GATT. Non-blocking (NimBLE owns its task).
void nav_ble_start(const nav_ble_cbs_t *cbs);

// Call after token verify to persist bond + stop advertising-as-new.
// (NimBLE bonding itself is handled in-stack; this just records pairing.)
void nav_ble_set_bonded(bool bonded);

// Current link state for the status icon.
bool nav_ble_connected(void);
