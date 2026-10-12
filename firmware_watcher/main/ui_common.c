#include "ui_common.h"

lv_obj_t *ui_round_button(lv_obj_t *parent, const char *sym, int x, int y, int d,
                          const lv_font_t *font, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, d, d);
    lv_obj_set_pos(b, x - d / 2, y - d / 2);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    // Frosted: the map shows through, so the button reads as a floating
    // control rather than a hole in the map.
    lv_obj_set_style_bg_color(b, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(b, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_90, LV_STATE_PRESSED);
    // Drop shadow, offset downwards: the light source is above the screen.
    lv_obj_set_style_shadow_width(b, 16, 0);
    lv_obj_set_style_shadow_color(b, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(b, LV_OPA_30, 0);
    lv_obj_set_style_shadow_offset_x(b, 0, 0);
    lv_obj_set_style_shadow_offset_y(b, 4, 0);
    // A press lifts the button: no shadow, slightly darker.
    lv_obj_set_style_shadow_width(b, 0, LV_STATE_PRESSED);
    lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x202124), 0);
    lv_obj_center(l);
    return b;
}