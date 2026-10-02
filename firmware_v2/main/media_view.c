#include "media_view.h"
#include "board.h"
#include "media_art.h"
#include "ui_common.h"
#include "esp_timer.h"
#include <stdio.h>

// Layout (480x480 round). Art centred above the middle so the transport row
// fits below; volume and back sit where zoom and the music button are on
// the map, so the hands already know where things are.
#define ART_CX 240
#define ART_CY 205
#define ART_D NAV_ART_PX
#define PROG_D 216   // progress ring, just outside the art
#define PROG_W 4
#define VOL_D 262    // volume arc, outside the progress ring
#define VOL_W 10
#define ROW_Y 388

#define COL_TRACK 0x2A2F36
#define COL_PROG 0xE8EAED
#define COL_VOL 0x2A6FDB
#define COL_TEXT 0xB0B8C0

#define VOL_SEND_MS 120      // throttle while dragging the arc
#define VOL_HOLD_MS 1500     // ignore phone volume echo after a local change
#define SYNC_RETRY_MS 8000
#define ART_STALL_MS 3000

// ---- persistent model ----
static nav_media_state_t s_st;
static bool s_have_state;
static uint32_t s_st_t0;
static bool s_connected;
static const lv_image_dsc_t *s_art;
static uint16_t s_art_id;
static uint32_t s_last_sync;
static int s_vol_pending = -1;
static uint32_t s_vol_sent_ms, s_vol_hold_until;
static bool s_vol_dragging;

// ---- UI (valid while the page exists) ----
static lv_obj_t *s_root, *s_img, *s_placeholder, *s_prog, *s_vol, *s_msg;
static lv_obj_t *s_btn_play, *s_lbl_play, *s_btn_prev, *s_btn_next, *s_btn_up, *s_btn_dn;
static void (*s_on_back)(void);
static void (*s_change_cb)(void);
static uint16_t s_seen_art;
static bool s_pending_change;
static int32_t s_prog_shown = -1;
static int s_msg_key = -1;
static char s_title[72], s_artist[56];
static lv_obj_t *s_lbl_title, *s_lbl_artist;

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool controls_live(void)
{
    return s_connected && nav_ble_media_ready();
}

static bool playing(void)
{
    return s_have_state && (s_st.flags & NAV_MEDIA_F_PLAYING);
}

static uint32_t position_now(void)
{
    uint32_t p = s_st.position_ms;
    if (playing()) p += now_ms() - s_st_t0;
    if (s_st.duration_ms && p > s_st.duration_ms) p = s_st.duration_ms;
    return p;
}

// ---------------------------------------------------------------- refresh

// Keeps 7-bit ASCII and Latin-1 (the font covers those); drops the rest,
// which is every non-Latin UTF-8 byte. Stops at the first space run.
static void sanitise(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    bool sp = false;
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 1 < cap; p++) {
        unsigned char c = *p;
        if (c >= 0x80 && c < 0xA0) continue;      // UTF-8 lead/continuation
        if (c < 0x20 || c == 0x7F) {
            sp = o > 0;
            continue;
        }
        if (c == ' ') {
            if (sp) continue;
            sp = true;
            c = ' ';
        } else {
            sp = false;
        }
        dst[o++] = (char)c;
    }
    while (o > 0 && dst[o - 1] == ' ') o--;
    dst[o] = '\0';
    if (!dst[0]) snprintf(dst, cap, "?");
}

void media_view_set_text(const char *title, const char *artist)
{
    sanitise(s_title, sizeof(s_title), title);
    sanitise(s_artist, sizeof(s_artist), artist);
    if (!s_lbl_title) return;
    lv_label_set_text(s_lbl_title, s_title);
    lv_label_set_text(s_lbl_artist, s_artist);
}

const char *media_view_title(void)
{
    return s_title;
}

static void refresh_art(void)
{
    if (!s_root) return;
    bool show = s_art && s_have_state && s_st.art_id && s_art_id == s_st.art_id;
    if (show) lv_image_set_src(s_img, s_art);
    lv_obj_set_hidden(s_img, !show);
    lv_obj_set_hidden(s_placeholder, show);
}

static void refresh_play(void)
{
    if (!s_root) return;
    lv_label_set_text(s_lbl_play, playing() ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

static void refresh_progress(void)
{
    if (!s_root) return;
    int32_t v = 0;
    if (s_have_state && s_st.duration_ms) {
        v = (int32_t)((uint64_t)position_now() * 360 / s_st.duration_ms);
    }
    if (v == s_prog_shown) return;
    s_prog_shown = v;
    lv_arc_set_value(s_prog, v);
}

static void refresh_volume(void)
{
    if (!s_root || s_vol_dragging || (int32_t)(now_ms() - s_vol_hold_until) < 0) return;
    if (s_have_state && lv_arc_get_value(s_vol) != s_st.volume) lv_arc_set_value(s_vol, s_st.volume);
}

static void refresh_controls(void)
{
    if (!s_root) return;
    bool live = controls_live();
    lv_obj_t *ctl[] = {s_btn_play, s_btn_prev, s_btn_next, s_btn_up, s_btn_dn, s_vol};
    for (size_t i = 0; i < sizeof(ctl) / sizeof(ctl[0]); i++) {
        if (live) {
            lv_obj_remove_state(ctl[i], LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(ctl[i], LV_STATE_DISABLED);
        }
    }
    int key;
    const char *txt = "";
    if (!s_connected) {
        key = 1, txt = "Phone not connected";
    } else if (!nav_ble_media_ready()) {
        key = 2, txt = "Update the Nav app for media controls";
    } else if (s_have_state && (s_st.flags & NAV_MEDIA_F_NO_ACCESS)) {
        key = 3, txt = "Allow Notification access in the Nav app";
    } else if (!s_have_state || !(s_st.flags & NAV_MEDIA_F_SESSION)) {
        key = 4, txt = "Nothing playing";
    } else {
        key = 0;
    }
    if (key == s_msg_key) return;
    s_msg_key = key;
    bool have_track = key == 0;
    lv_label_set_text(s_msg, txt);
    lv_obj_set_hidden(s_msg, have_track);
    lv_obj_set_hidden(s_lbl_title, !have_track);
    lv_obj_set_hidden(s_lbl_artist, !have_track);
}

// ---------------------------------------------------------------- input

static void back_cb(lv_event_t *e)
{
    (void)e;
    if (s_on_back) s_on_back();
}

static void play_cb(lv_event_t *e)
{
    (void)e;
    nav_ble_media_cmd(NAV_MCMD_PLAY_PAUSE, 0);
    // Optimistic: flip now, the phone's STATE confirms a moment later.
    if (s_have_state) {
        s_st.position_ms = position_now();
        s_st_t0 = now_ms();
        s_st.flags ^= NAV_MEDIA_F_PLAYING;
    }
    refresh_play();
}

static void prev_cb(lv_event_t *e)
{
    (void)e;
    nav_ble_media_cmd(NAV_MCMD_PREV, 0);
}

static void next_cb(lv_event_t *e)
{
    (void)e;
    nav_ble_media_cmd(NAV_MCMD_NEXT, 0);
}

static void vol_up_cb(lv_event_t *e)
{
    (void)e;
    nav_ble_media_cmd(NAV_MCMD_VOL_UP, 0);
}

static void vol_dn_cb(lv_event_t *e)
{
    (void)e;
    nav_ble_media_cmd(NAV_MCMD_VOL_DOWN, 0);
}

static void vol_arc_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    uint32_t t = now_ms();
    if (code == LV_EVENT_PRESSED) {
        s_vol_dragging = true;
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        s_vol_pending = lv_arc_get_value(s_vol);
        if (t - s_vol_sent_ms >= VOL_SEND_MS) {
            nav_ble_media_cmd(NAV_MCMD_SET_VOLUME, (uint8_t)s_vol_pending);
            s_vol_sent_ms = t;
            s_vol_pending = -1;
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_vol_dragging = false;
        nav_ble_media_cmd(NAV_MCMD_SET_VOLUME, (uint8_t)lv_arc_get_value(s_vol));
        s_vol_pending = -1;
        s_vol_sent_ms = t;
        s_vol_hold_until = t + VOL_HOLD_MS;
    }
}

// ---------------------------------------------------------------- build

static lv_obj_t *make_arc(lv_obj_t *parent, int d, int w, uint32_t ind_col)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_set_size(a, d, d);
    lv_obj_set_pos(a, ART_CX - d / 2, ART_CY - d / 2);
    lv_obj_set_style_arc_width(a, w, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, w, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, lv_color_hex(ind_col), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(a, 0, LV_PART_MAIN);
    return a;
}

void media_view_create(lv_obj_t *parent, void (*on_back)(void))
{
    s_on_back = on_back;
    lv_obj_set_style_bg_color(parent, lv_color_hex(MEDIA_ART_BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(parent, false);
    s_root = parent;

    // Album art (pre-rounded at decode time) + placeholder.
    s_placeholder = lv_obj_create(parent);
    lv_obj_remove_style_all(s_placeholder);
    lv_obj_set_size(s_placeholder, ART_D, ART_D);
    lv_obj_set_pos(s_placeholder, ART_CX - ART_D / 2, ART_CY - ART_D / 2);
    lv_obj_set_style_radius(s_placeholder, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_placeholder, lv_color_hex(COL_TRACK), 0);
    lv_obj_set_style_bg_opa(s_placeholder, LV_OPA_COVER, 0);
    lv_obj_set_clickable(s_placeholder, false);
    lv_obj_t *note = lv_label_create(s_placeholder);
    lv_label_set_text(note, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(note, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(note, lv_color_hex(0x5F6368), 0);
    lv_obj_center(note);

    s_img = lv_image_create(parent);
    lv_obj_set_pos(s_img, ART_CX - ART_D / 2, ART_CY - ART_D / 2);
    lv_obj_set_size(s_img, ART_D, ART_D);
    lv_obj_set_hidden(s_img, true);

    // Progress ring: full circle from 12 o'clock, display only.
    s_prog = make_arc(parent, PROG_D, PROG_W, COL_PROG);
    lv_arc_set_rotation(s_prog, 270);
    lv_arc_set_bg_angles(s_prog, 0, 360);
    lv_arc_set_range(s_prog, 0, 360);
    lv_arc_set_value(s_prog, 0);
    lv_obj_remove_style(s_prog, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(s_prog, false);
    lv_obj_set_style_arc_rounded(s_prog, false, LV_PART_INDICATOR);

    // Volume arc: 270 deg with the gap at the bottom, draggable on the ring.
    s_vol = make_arc(parent, VOL_D, VOL_W, COL_VOL);
    lv_arc_set_bg_angles(s_vol, 135, 45);
    lv_arc_set_range(s_vol, 0, 100);
    lv_arc_set_value(s_vol, s_have_state ? s_st.volume : 0);
    lv_obj_set_adv_hittest(s_vol, true);  // only the ring itself is touchable
    lv_obj_set_style_bg_color(s_vol, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_vol, 4, LV_PART_KNOB);
    lv_obj_set_style_opa(s_vol, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_vol, vol_arc_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_vol, vol_arc_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_vol, vol_arc_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_vol, vol_arc_cb, LV_EVENT_PRESS_LOST, NULL);

    const lv_font_t *f = &lv_font_montserrat_28;
    ui_round_button(parent, LV_SYMBOL_LEFT, 70, 240, 58, f, back_cb);
    s_btn_up = ui_round_button(parent, LV_SYMBOL_PLUS, 410, 200, 58, f, vol_up_cb);
    s_btn_dn = ui_round_button(parent, LV_SYMBOL_MINUS, 410, 280, 58, f, vol_dn_cb);
    s_btn_prev = ui_round_button(parent, LV_SYMBOL_PREV, 150, ROW_Y, 58, f, prev_cb);
    s_btn_next = ui_round_button(parent, LV_SYMBOL_NEXT, 330, ROW_Y, 58, f, next_cb);
    s_btn_play = ui_round_button(parent, LV_SYMBOL_PLAY, 240, ROW_Y, 76, &lv_font_montserrat_40, play_cb);
    s_lbl_play = lv_obj_get_child(s_btn_play, 0);

    // Title + artist sit above the art: the progress and volume rings leave
    // no room under it. The status message takes the title's place.
    s_lbl_title = lv_label_create(parent);
    lv_obj_set_width(s_lbl_title, 360);
    lv_obj_set_style_text_align(s_lbl_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_lbl_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_lbl_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_title, lv_color_white(), 0);
    lv_obj_align(s_lbl_title, LV_ALIGN_TOP_MID, 0, 34);
    lv_label_set_text(s_lbl_title, s_title);

    s_lbl_artist = lv_label_create(parent);
    lv_obj_set_width(s_lbl_artist, 360);
    lv_obj_set_style_text_align(s_lbl_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_lbl_artist, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_lbl_artist, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_artist, lv_color_hex(COL_TEXT), 0);
    lv_obj_align(s_lbl_artist, LV_ALIGN_TOP_MID, 0, 55);
    lv_label_set_text(s_lbl_artist, s_artist);

    s_msg = lv_label_create(parent);
    lv_obj_set_width(s_msg, 380);
    lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_msg, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_msg, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_msg, lv_color_hex(COL_TEXT), 0);
    lv_obj_align(s_msg, LV_ALIGN_TOP_MID, 0, 34);
    lv_label_set_text(s_msg, "");

    s_prog_shown = -1;
    s_msg_key = -1;
    refresh_art();
    refresh_play();
    refresh_progress();
    refresh_controls();
}

void media_view_set_change_cb(void (*cb)(void))
{
    s_change_cb = cb;
}

void media_view_destroy(void)
{
    // Objects are deleted by the caller cleaning the screen.
    s_root = s_img = s_placeholder = s_prog = s_vol = s_msg = NULL;
    s_lbl_title = s_lbl_artist = NULL;
    s_btn_play = s_lbl_play = s_btn_prev = s_btn_next = s_btn_up = s_btn_dn = NULL;
    s_vol_dragging = false;
}

bool media_view_active(void)
{
    return s_root != NULL;
}

// ---------------------------------------------------------------- updates

void media_view_set_state(const nav_media_state_t *st)
{
    // The phone resends STATE about once a second while playing; a toast is
    // only wanted when the track really changed (art id changes with it).
    bool changed = st->art_id != 0 && s_have_state && st->art_id != s_st.art_id;
    if (!s_have_state) s_seen_art = 0;
    if (changed) s_pending_change = true;
    s_st = *st;
    s_have_state = true;
    s_st_t0 = now_ms();
    s_seen_art = st->art_id;
    s_prog_shown = -1;
    refresh_art();
    refresh_play();
    refresh_volume();
    refresh_progress();
    refresh_controls();
    if (s_msg_key == 0) {  // a new track: labels are current again
        lv_label_set_text(s_lbl_title, s_title);
        lv_label_set_text(s_lbl_artist, s_artist);
    }
}

void media_view_set_link(bool connected)
{
    s_connected = connected;
    if (!connected) s_have_state = false;
    refresh_art();
    refresh_play();
    refresh_controls();
}

void media_view_tick(void)
{
    uint32_t t = now_ms();

    const lv_image_dsc_t *img;
    uint16_t id;
    if (media_art_poll(&img, &id)) {
        s_art = img;
        s_art_id = id;
        refresh_art();
    }

    // Missing or stalled art for the current track: ask the phone again
    // (after a grace period: the phone sends art right after the STATE).
    if (controls_live() && s_have_state && s_st.art_id && s_st.art_id != s_art_id &&
        t - s_st_t0 > ART_STALL_MS && t - s_last_sync > SYNC_RETRY_MS &&
        (media_art_rx_id() != s_st.art_id || media_art_rx_idle_ms() > ART_STALL_MS)) {
        s_last_sync = t;
        nav_ble_media_cmd(NAV_MCMD_SYNC, 0);
    }

    // Track changed while another screen is up (map mode): report it there.
    if (s_pending_change && !s_root) {
        s_pending_change = false;
        if (s_change_cb) s_change_cb();
    }
    if (!s_root) return;

    if (s_vol_pending >= 0 && t - s_vol_sent_ms >= VOL_SEND_MS) {
        nav_ble_media_cmd(NAV_MCMD_SET_VOLUME, (uint8_t)s_vol_pending);
        s_vol_sent_ms = t;
        s_vol_pending = -1;
    }
    refresh_progress();
    refresh_volume();
    refresh_controls();  // subscription state can change without an event
}
