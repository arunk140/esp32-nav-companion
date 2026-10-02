#pragma once
#include <stdint.h>

// QR rendering for the pairing screen.
//
// The QR text is byte-for-byte what firmware v1 put on screen, because the
// Android scanner parses it as JSON:
//   {"n":"<name>","m":"<MAC>","s":"<svc uuid>","t":"<6-digit token>"}
#include "esp_err.h"

// Build the payload string (also used for logging). Returns its length.
int qr_payload(char *out, int out_len);

// Draw the pairing QR centred at (cx, top). Black modules on white, with a
// quiet zone, which is what every scanner expects.
esp_err_t qr_draw(int cx, int top);
