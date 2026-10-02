#pragma once
#include "lvgl.h"

// QR pairing screen. Payload (same as v1):
//   {"n":"NAV-XXXX","m":"AA:BB:CC:DD:EE:FF","s":"<svc uuid>","t":"123456"}
void ui_pair_create(lv_obj_t *parent, lv_event_cb_t on_browse);
