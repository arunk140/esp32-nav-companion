#include "media_view.h"
#include "board.h"
#include "media_art.h"
#include "ui_common.h"
#include "esp_timer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// Layout (480x480 round), top to bottom: a volume arc along the upper rim,
// the album art inside its progress ring, title and artist, then the
// transport row. Black throughout, like the minimal display, with one colour
// taken from the album art for the progress ring.
#define ART_CX 240
#define ART_CY 162
#define ART_D NAV_ART_PX
#define PROG_D 222   // progress ring, just outside the art
#define PROG_W 5
#define VOL_D 448    // volume arc, on the rim; its centre is the screen's
#define VOL_W 8
#define VOL_A0 212   // LVGL angles: 270 is the top of the screen
#define VOL_A1 328
#define TITLE_Y 288
#define ARTIST_Y 318
#define ROW_Y 394
#define TEXT_W 290

#define COL_TRACK 0x24282D
#define COL_PROG 0xE8EAED    // progress ring until the art gives a colour
#define COL_VOL 0xE8EAED
#define COL_TEXT 0x9AA0A6
#define COL_GHOST 0xE8EAED   // icons of the buttons that have no disc

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
static lv_obj_t *s_btn_play, *s_lbl_play, *s_btn_prev, *s_btn_next;
static uint16_t s_accent_for;  // art id the ring colour was taken from
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

// Copies at most `max` characters, cut at a word boundary where there is
// one, with "..." when it had to be shortened.
static void chop(char *dst, size_t cap, const char *src, size_t max)
{
    size_t n = strlen(src);
    if (n <= max) {
        snprintf(dst, cap, "%s", src);
        return;
    }
    for (size_t i = max; i > max / 2; i--) {
        if (src[i] == ' ') {
            n = i;
            break;
        }
    }
    snprintf(dst, cap, "%.*s...", (int)n, src);
}

void media_view_set_text(const char *title, const char *artist)
{
    // Keep the full text for the map toast, which chops to its own length.
    sanitise(s_title, sizeof(s_title), title);
    sanitise(s_artist, sizeof(s_artist), artist);
    if (!s_lbl_title) return;
    // The title scrolls when it is too long for its line; the artist is cut.
    char a[40];
    chop(a, sizeof(a), s_artist, 30);
    lv_label_set_text(s_lbl_title, s_title);
    lv_label_set_text(s_lbl_artist, a);
}

const char *media_view_title(void)
{
    return s_title;
}

// One colour for the progress ring, taken from the album art: its average,
// pushed up to a brightness that reads on black. Art with no colour to speak
// of (greys, near-black) keeps the neutral ring.
static lv_color_t art_accent(const lv_image_dsc_t *img)
{
    if (!img || img->header.cf != LV_COLOR_FORMAT_RGB565) return lv_color_hex(COL_PROG);
    const uint16_t *px = (const uint16_t *)img->data;
    uint32_t n = (uint32_t)img->header.w * img->header.h;
    uint32_t r = 0, g = 0, b = 0, cnt = 0;
    for (uint32_t i = 0; i < n; i += 7) {
        uint16_t p = px[i];
        int pr = (p >> 11) << 3, pg = ((p >> 5) & 0x3F) << 2, pb = (p & 0x1F) << 3;
        if (pr + pg + pb < 60) continue;  // the masked corners and dark areas say nothing
        r += pr, g += pg, b += pb, cnt++;
    }
    if (cnt < 50) return lv_color_hex(COL_PROG);
    r /= cnt, g /= cnt, b /= cnt;
    uint32_t hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
    uint32_t lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
    if (hi - lo < 28) return lv_color_hex(COL_PROG);
    // Widen the gap between the channels a little, then lift the brightest to 235.
    r = r > lo / 2 ? r - lo / 2 : 0, g = g > lo / 2 ? g - lo / 2 : 0, b = b > lo / 2 ? b - lo / 2 : 0;
    hi -= lo / 2;
    return lv_color_make((uint8_t)(r * 235 / hi), (uint8_t)(g * 235 / hi), (uint8_t)(b * 235 / hi));
}

static void refresh_art(void)
{
    if (!s_root) return;
    bool show = s_art && s_have_state && s_st.art_id && s_art_id == s_st.art_id;
    if (show) lv_image_set_src(s_img, s_art);
    if (show && s_accent_for != s_art_id) {
        s_accent_for = s_art_id;
        lv_obj_set_style_arc_color(s_prog, art_accent(s_art), LV_PART_INDICATOR);
    } else if (!show && s_accent_for) {
        s_accent_for = 0;
        lv_obj_set_style_arc_color(s_prog, lv_color_hex(COL_PROG), LV_PART_INDICATOR);
    }
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
    lv_obj_t *ctl[] = {s_btn_play, s_btn_prev, s_btn_next, s_vol};
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

static void swipe_back_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (indev && lv_indev_get_gesture_dir(indev) == LV_DIR_RIGHT && s_on_back) s_on_back();
}

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

static lv_obj_t *make_arc(lv_obj_t *parent, int cx, int cy, int d, int w, uint32_t ind_col)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_set_size(a, d, d);
    lv_obj_set_pos(a, cx - d / 2, cy - d / 2);
    lv_obj_set_style_arc_width(a, w, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, w, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, lv_color_hex(ind_col), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(a, 0, LV_PART_MAIN);
    return a;
}

// A round button of this page. `filled`: a white disc with a dark icon, for
// the one main action. Otherwise only the icon, with a faint disc while it is
// pressed, so the page stays black around the art.
static lv_obj_t *icon_button(lv_obj_t *parent, const char *sym, int x, int y, int d,
                             const lv_font_t *font, uint32_t icon_col, bool filled, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, d, d);
    lv_obj_set_pos(b, x - d / 2, y - d / 2);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(b, filled ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(b, filled ? LV_OPA_70 : LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_opa(b, LV_OPA_30, LV_STATE_DISABLED);
    lv_obj_set_ext_click_area(b, 8);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(filled ? 0x101214 : icon_col), 0);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *text_line(lv_obj_t *parent, const lv_font_t *font, uint32_t col, int y,
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

// A small dim icon at `deg` on the rim, marking an end of the volume arc.
static void rim_icon(lv_obj_t *parent, const char *sym, float deg)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT), 0);
    float a = deg * 3.14159265f / 180.0f;
    float r = VOL_D / 2.0f - VOL_W / 2.0f;
    lv_obj_align(l, LV_ALIGN_CENTER, (int)(cosf(a) * r), (int)(sinf(a) * r));
}

void media_view_create(lv_obj_t *parent, void (*on_back)(void))
{
    s_on_back = on_back;
    // A swipe to the right goes back, the way a swipe to the left got here
    // from the minimal display. The callback only asks for the screen change.
    lv_obj_add_event_cb(parent, swipe_back_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_set_style_bg_color(parent, lv_color_hex(MEDIA_ART_BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(parent, false);
    s_root = parent;

    // Volume: an arc along the upper rim, dragged with a thumb, quiet at the
    // left end and loud at the right.
    s_vol = make_arc(parent, BOARD_LCD_W / 2, BOARD_LCD_H / 2, VOL_D, VOL_W, COL_VOL);
    lv_arc_set_bg_angles(s_vol, VOL_A0, VOL_A1);
    lv_arc_set_range(s_vol, 0, 100);
    lv_arc_set_value(s_vol, s_have_state ? s_st.volume : 0);
    lv_obj_set_adv_hittest(s_vol, true);  // only the ring itself is touchable
    lv_obj_set_ext_click_area(s_vol, 14);
    lv_obj_set_style_bg_color(s_vol, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_vol, 5, LV_PART_KNOB);
    lv_obj_set_style_opa(s_vol, LV_OPA_30, LV_STATE_DISABLED);
    // A drag along the arc is sideways: it must not also count as the swipe
    // that leaves this page.
    lv_obj_set_gesture_bubble(s_vol, false);
    lv_obj_add_event_cb(s_vol, vol_arc_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_vol, vol_arc_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_vol, vol_arc_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_vol, vol_arc_cb, LV_EVENT_PRESS_LOST, NULL);
    rim_icon(parent, LV_SYMBOL_VOLUME_MID, VOL_A0 - 7.0f);
    rim_icon(parent, LV_SYMBOL_VOLUME_MAX, VOL_A1 + 7.0f);

    // Album art (pre-rounded at decode time) + placeholder.
    s_placeholder = lv_obj_create(parent);
    lv_obj_remove_style_all(s_placeholder);
    lv_obj_set_size(s_placeholder, ART_D, ART_D);
    lv_obj_set_pos(s_placeholder, ART_CX - ART_D / 2, ART_CY - ART_D / 2);
    lv_obj_set_style_radius(s_placeholder, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_placeholder, lv_color_hex(0x16191D), 0);
    lv_obj_set_style_bg_opa(s_placeholder, LV_OPA_COVER, 0);
    lv_obj_set_clickable(s_placeholder, false);
    lv_obj_t *note = lv_label_create(s_placeholder);
    lv_label_set_text(note, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(note, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(note, lv_color_hex(0x3C4043), 0);
    lv_obj_center(note);

    s_img = lv_image_create(parent);
    lv_obj_set_pos(s_img, ART_CX - ART_D / 2, ART_CY - ART_D / 2);
    lv_obj_set_size(s_img, ART_D, ART_D);
    lv_obj_set_hidden(s_img, true);

    // Progress ring: full circle from 12 o'clock, display only.
    s_prog = make_arc(parent, ART_CX, ART_CY, PROG_D, PROG_W, COL_PROG);
    lv_arc_set_rotation(s_prog, 270);
    lv_arc_set_bg_angles(s_prog, 0, 360);
    lv_arc_set_range(s_prog, 0, 360);
    lv_arc_set_value(s_prog, 0);
    lv_obj_remove_style(s_prog, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(s_prog, false);
    s_accent_for = 0;

    // Title, artist. The status message takes the title's place.
    s_lbl_title = text_line(parent, &lv_font_montserrat_20, 0xFFFFFF, TITLE_Y, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(s_lbl_title, s_title);
    s_lbl_artist = text_line(parent, &lv_font_montserrat_14, COL_TEXT, ARTIST_Y, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_artist, s_artist);
    s_msg = text_line(parent, &lv_font_montserrat_14, COL_TEXT, TITLE_Y + 6, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_msg, "");

    // Transport: the one white disc is play, flanked by plain icons.
    const lv_font_t *f = &lv_font_montserrat_28;
    s_btn_prev = icon_button(parent, LV_SYMBOL_PREV, 146, ROW_Y, 60, f, COL_GHOST, false, prev_cb);
    s_btn_next = icon_button(parent, LV_SYMBOL_NEXT, 334, ROW_Y, 60, f, COL_GHOST, false, next_cb);
    s_btn_play = icon_button(parent, LV_SYMBOL_PLAY, 240, ROW_Y, 76, &lv_font_montserrat_28, 0, true, play_cb);
    s_lbl_play = lv_obj_get_child(s_btn_play, 0);
    // Back, where the music button is on the map. A swipe right does the same.
    icon_button(parent, LV_SYMBOL_LEFT, 52, 240, 56, &lv_font_montserrat_20, COL_TEXT, false, back_cb);

    s_prog_shown = -1;
    s_msg_key = -1;
    media_view_set_text(s_title, s_artist);
    refresh_art();
    refresh_play();
    refresh_progress();
    refresh_controls();
}

void media_view_set_change_cb(void (*cb)(void))
{
    s_change_cb = cb;
}

static void swipe_back_cb(lv_event_t *e);

void media_view_destroy(void)
{
    // The screen object outlives this view, so its gesture hook has to go.
    lv_obj_remove_event_cb(lv_screen_active(), swipe_back_cb);
    // Objects are deleted by the caller cleaning the screen.
    s_root = s_img = s_placeholder = s_prog = s_vol = s_msg = NULL;
    s_lbl_title = s_lbl_artist = NULL;
    s_btn_play = s_lbl_play = s_btn_prev = s_btn_next = NULL;
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
        media_view_set_text(s_title, s_artist);
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
