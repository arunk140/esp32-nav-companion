#include "input/touch.h"
#include "Touch_Driver/GT911.h"  // tp
#include "esp_lcd_touch.h"
#include "esp_log.h"

#define SAFE_RADIUS 225  // round bezel: corners are under glass, not screen

static touch_sample_t s_s;
static uint16_t s_x[TOUCH_MAX_POINTS];
static uint16_t s_y[TOUCH_MAX_POINTS];

void touch_poll(void)
{
    bool prev_down = s_s.down;

    s_s.just_down = false;
    s_s.just_up = false;
    s_s.n = 0;
    s_s.down = false;

    esp_lcd_touch_read_data(tp);
    uint8_t cnt = 0;
    esp_lcd_touch_get_coordinates(tp, s_x, s_y, NULL, &cnt, TOUCH_MAX_POINTS);
    if (cnt == 0) {
        s_s.just_up = prev_down;
        return;
    }

    // The panel is round and the corners sit under the bezel. Treat a touch
    // outside the safe circle as no contact at all, so a stray hit on the
    // bezel does not start a drag or a button press.
    int cx = s_x[0] - 240;
    int cy = s_y[0] - 240;
    if (cx * cx + cy * cy > SAFE_RADIUS * SAFE_RADIUS) {
        s_s.just_up = prev_down;
        return;
    }

    for (int i = 0; i < cnt && i < TOUCH_MAX_POINTS; i++) {
        s_s.x[i] = s_x[i];
        s_s.y[i] = s_y[i];
    }
    s_s.n = cnt;
    s_s.down = true;
    s_s.just_down = !prev_down;
}

const touch_sample_t *touch_get(void)
{
    return &s_s;
}
