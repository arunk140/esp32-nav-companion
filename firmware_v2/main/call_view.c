#include "call_view.h"
#include "ble_service.h"
#include "board.h"
#include <stdio.h>
#include <string.h>

// A dark full-screen page, so it reads as an interruption. Three targets:
// Answer (green), Reject (red), Dismiss (neutral, smaller, in the middle).
// "Dismiss" only closes the page: the call keeps ringing on the phone.

#define COL_BG 0x101418
#define COL_TITLE 0x9AA0A6
#define COL_NAME 0xFFFFFF
#define COL_NUM 0xC7CCD1
#define COL_ANSWER 0x1E8E3E
#define COL_REJECT 0xD93025
#define COL_DISMISS 0x5F6368

#define BTN_D 86
#define DISMISS_D 68
#define BTN_Y 362
#define BTN_SPREAD 118  // Answer/Reject offset from the screen centre

static char s_name[NAV_CALL_TEXT_MAX + 1];
static char s_number[NAV_CALL_TEXT_MAX + 1];

static lv_obj_t *s_lbl_name, *s_lbl_num;
static void (*s_on_answer)(void);
static void (*s_on_reject)(void);
static void (*s_on_dismiss)(void);

// Same Latin-only rule as the media and nav text: drop what the board font
// cannot draw, squeeze control bytes to spaces, trim.
static void latin_copy(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && o < cap - 1; p++) {
        if (*p >= 0x80 && *p < 0xA0) continue;
        if (*p < 0x20 || *p == 0x7F) {
            if (o) dst[o++] = ' ';
            continue;
        }
        dst[o++] = (char)*p;
    }
    while (o > 0 && dst[o - 1] == ' ') o--;
    dst[o] = '\0';
}

static void refresh_text(void)
{
    if (!s_lbl_name) return;
    lv_label_set_text(s_lbl_name, s_name[0] ? s_name : "Unknown caller");
    lv_label_set_text(s_lbl_num, s_number);
}

static void mk_btn(lv_obj_t *parent, const char *sym, int x, int y, int d,
                   uint32_t col, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, d, d);
    lv_obj_set_pos(b, x - d / 2, y - d / 2);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(col), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(col), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(b, 18, 0);
    lv_obj_set_style_shadow_color(b, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(b, LV_OPA_40, 0);
    lv_obj_set_style_shadow_offset_x(b, 0, 0);
    lv_obj_set_style_shadow_offset_y(b, 4, 0);
    lv_obj_set_style_shadow_width(b, 0, LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);
}

static void answer_cb(lv_event_t *e)
{
    (void)e;
    if (s_on_answer) s_on_answer();
}

static void reject_cb(lv_event_t *e)
{
    (void)e;
    if (s_on_reject) s_on_reject();
}

static void dismiss_cb(lv_event_t *e)
{
    (void)e;
    if (s_on_dismiss) s_on_dismiss();
}

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t col, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, 360);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

void call_view_create(lv_obj_t *parent, void (*on_answer)(void),
                      void (*on_reject)(void), void (*on_dismiss)(void))
{
    s_on_answer = on_answer;
    s_on_reject = on_reject;
    s_on_dismiss = on_dismiss;

    lv_obj_set_style_bg_color(parent, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(parent, false);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "Incoming call");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TITLE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 74);

    s_lbl_name = mk_label(parent, &lv_font_montserrat_40, COL_NAME, 128);
    s_lbl_num = mk_label(parent, &lv_font_montserrat_20, COL_NUM, 190);

    mk_btn(parent, LV_SYMBOL_CALL, BOARD_LCD_W / 2 - BTN_SPREAD, BTN_Y, BTN_D,
           COL_ANSWER, answer_cb);
    mk_btn(parent, LV_SYMBOL_DOWN, BOARD_LCD_W / 2, BTN_Y, DISMISS_D,
           COL_DISMISS, dismiss_cb);
    mk_btn(parent, LV_SYMBOL_CLOSE, BOARD_LCD_W / 2 + BTN_SPREAD, BTN_Y, BTN_D,
           COL_REJECT, reject_cb);

    refresh_text();
}

void call_view_destroy(void)
{
    s_lbl_name = s_lbl_num = NULL;
    s_on_answer = s_on_reject = s_on_dismiss = NULL;
}

void call_view_set_caller(const char *name, const char *number)
{
    latin_copy(s_name, sizeof(s_name), name ? name : "");
    latin_copy(s_number, sizeof(s_number), number ? number : "");
    refresh_text();
}

void call_view_clear(void)
{
    s_name[0] = '\0';
    s_number[0] = '\0';
    refresh_text();
}
