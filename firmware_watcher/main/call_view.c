#include "call_view.h"
#include "ble_service.h"
#include "board.h"
#include <stdio.h>
#include <string.h>

// Incoming call, black like the minimal display. From the top: a small
// "Incoming call", a disc with the caller's initial and a ring that pulses out
// of it while the phone rings, the name, the number, then the buttons.
// Decline is on the left in red and Answer on the right in green, the order
// phones use. The small arrow between them only closes this page: the call
// keeps ringing on the phone.

#define COL_BG 0x000000
#define COL_TITLE 0x9AA0A6
#define COL_NAME 0xFFFFFF
#define COL_NUM 0x9AA0A6
#define COL_ANSWER 0x1E8E3E
#define COL_REJECT 0xD93025

#define AVATAR_D UI_SC(112)
#define AVATAR_Y UI_SC(148)   // centre
#define PULSE_GROW UI_SC(46)  // how far the ring travels out of the disc, px
#define PULSE_MS 1500
#define NAME_Y UI_SC(222)
#define NUM_Y UI_SC(262)
#define BTN_D UI_SC(84)
#define DISMISS_D UI_SC(56)
#define BTN_Y UI_SC(344)
#define BTN_SPREAD UI_SC(108)  // Decline/Answer offset from the screen centre
#define TEXT_W UI_SC(320)

static char s_name[NAV_CALL_TEXT_MAX + 1];
static char s_number[NAV_CALL_TEXT_MAX + 1];

static lv_obj_t *s_lbl_name, *s_lbl_num, *s_lbl_initial, *s_avatar;
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

// A colour for the caller's disc, the same one every time for the same name.
static uint32_t avatar_colour(const char *key)
{
    static const uint32_t pal[] = {0x3B6FD9, 0x2E9E8F, 0x8E5BD0, 0xC7783A, 0xC2527E, 0x4E8F3A};
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)key; *p; p++) h = (h ^ *p) * 16777619u;
    return pal[h % (sizeof(pal) / sizeof(pal[0]))];
}

static void refresh_text(void)
{
    if (!s_lbl_name) return;
    // A name leads when there is one, with the number under it. With only a
    // number, the number leads. With neither, say so.
    bool named = s_name[0] != '\0';
    const char *lead = named ? s_name : s_number[0] ? s_number : "Unknown caller";
    lv_label_set_text(s_lbl_name, lead);
    lv_label_set_text(s_lbl_num, named ? s_number : "");
    // The disc: the name's first letter, or a handset for a bare number.
    char ini[2] = {0, 0};
    unsigned char c = (unsigned char)s_name[0];
    if (named && c < 0x80 && c > ' ') ini[0] = (char)((c >= 'a' && c <= 'z') ? c - 32 : c);
    lv_label_set_text(s_lbl_initial, ini[0] ? ini : LV_SYMBOL_CALL);
    lv_obj_set_style_bg_color(s_avatar, lv_color_hex(named ? avatar_colour(s_name) : 0x3C4043), 0);
}

// A disc button. `rot_deci`: turn the icon by that many tenths of a degree
// (the handset laid flat is the usual sign for declining a call).
static void mk_btn(lv_obj_t *parent, const char *sym, int x, int y, int d, uint32_t col,
                   bool filled, int rot_deci, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, d, d);
    lv_obj_set_pos(b, x - d / 2, y - d / 2);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(filled ? col : 0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(b, filled ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(b, filled ? LV_OPA_70 : LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(b, 10);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(l, filled ? lv_color_white() : lv_color_hex(col), 0);
    lv_obj_center(l);
    if (rot_deci) {
        lv_obj_set_style_transform_pivot_x(l, lv_pct(50), 0);
        lv_obj_set_style_transform_pivot_y(l, lv_pct(50), 0);
        lv_obj_set_style_transform_rotation(l, rot_deci, 0);
    }
}

static void caption(lv_obj_t *parent, const char *txt, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_TITLE), 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, x - BOARD_LCD_W / 2, y);
}

// The ring: starts on the disc's edge, grows and fades, then starts again.
static void pulse_cb(void *obj, int32_t v)
{
    int d = AVATAR_D + PULSE_GROW * 2 * v / 100;
    lv_obj_set_size(obj, d, d);
    lv_obj_set_pos(obj, BOARD_LCD_W / 2 - d / 2, AVATAR_Y - d / 2);
    lv_obj_set_style_border_opa(obj, (lv_opa_t)(LV_OPA_60 * (100 - v) / 100), 0);
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

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t col, int y,
                          lv_label_long_mode_t mode)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, TEXT_W);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, mode);
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
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TITLE), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, UI_SC(46));

    lv_obj_t *ring = lv_obj_create(parent);
    lv_obj_remove_style_all(ring);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(ring, lv_color_white(), 0);
    lv_obj_set_style_border_width(ring, 3, 0);
    lv_obj_set_clickable(ring, false);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ring);
    lv_anim_set_exec_cb(&a, pulse_cb);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_duration(&a, PULSE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    s_avatar = lv_obj_create(parent);
    lv_obj_remove_style_all(s_avatar);
    lv_obj_set_size(s_avatar, AVATAR_D, AVATAR_D);
    lv_obj_set_pos(s_avatar, BOARD_LCD_W / 2 - AVATAR_D / 2, AVATAR_Y - AVATAR_D / 2);
    lv_obj_set_style_radius(s_avatar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_avatar, LV_OPA_COVER, 0);
    lv_obj_set_clickable(s_avatar, false);
    s_lbl_initial = lv_label_create(s_avatar);
    lv_obj_set_style_text_font(s_lbl_initial, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_lbl_initial, lv_color_white(), 0);
    lv_obj_center(s_lbl_initial);

    // A long name scrolls; the number is short enough to cut.
    s_lbl_name = mk_label(parent, &lv_font_montserrat_28, COL_NAME, NAME_Y, LV_LABEL_LONG_SCROLL_CIRCULAR);
    s_lbl_num = mk_label(parent, &lv_font_montserrat_20, COL_NUM, NUM_Y, LV_LABEL_LONG_DOT);

    int cx = BOARD_LCD_W / 2;
    mk_btn(parent, LV_SYMBOL_CALL, cx - BTN_SPREAD, BTN_Y, BTN_D, COL_REJECT, true, 1350, reject_cb);
    mk_btn(parent, LV_SYMBOL_DOWN, cx, BTN_Y, DISMISS_D, COL_TITLE, false, 0, dismiss_cb);
    mk_btn(parent, LV_SYMBOL_CALL, cx + BTN_SPREAD, BTN_Y, BTN_D, COL_ANSWER, true, 0, answer_cb);
    caption(parent, "Decline", cx - BTN_SPREAD, BTN_Y + BTN_D / 2 + 8);
    caption(parent, "Answer", cx + BTN_SPREAD, BTN_Y + BTN_D / 2 + 8);

    refresh_text();
}

void call_view_destroy(void)
{
    s_lbl_name = s_lbl_num = s_lbl_initial = s_avatar = NULL;
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
