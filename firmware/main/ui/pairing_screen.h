#pragma once
#include "lvgl.h"

// Pairing screen: QR + device name + instructions + Skip button. Shown on boot
// when settings_get_paired() is false. QR payload:
//   {"n":"NAV-XXXX","m":"AA:BB:CC:DD:EE:FF","s":"<svc-uuid>","t":"123456"}
// MAC is the BLE MAC; filled by M3. M1 shows name + token + placeholder MAC.

// Called when the user taps Skip -> browse-only map (drag + pinch, no location).
typedef void (*pairing_skip_cb_t)(void *arg);

// Builds the payload string into out (out_len bytes). Returns false if truncated.
bool pairing_build_payload(char *out, size_t out_len);

// Creates the full pairing screen as a child of parent. Returns the screen obj.
lv_obj_t *pairing_screen_create(lv_obj_t *parent, pairing_skip_cb_t on_skip, void *arg);
