#include "ui_common.h"

lv_obj_t *ui_round_button(lv_obj_t *parent, const char *sym, int x, int y, int d,
                          const lv_font_t *font, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, d, d);
    lv_obj_set_pos(b, x - d / 2, y - d / 2);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0xDDE3EA), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x202124), 0);
    lv_obj_center(l);
    return b;
}
