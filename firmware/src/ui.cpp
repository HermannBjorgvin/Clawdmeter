#include "ui.h"
#include "splash.h"
#include "audio.h"
#include <lvgl.h>
#include "logo.h"
#include "icons.h"
#include "display_cfg.h"

// Custom fonts (scaled for 314 PPI, ~1.9x from original 165 PPI)
LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_mono_18);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_mono_32);

// Anthropic brand palette — design tokens live in theme.h
#include "theme.h"
#define COL_BG        THEME_BG
#define COL_PANEL     THEME_PANEL
#define COL_TEXT      THEME_TEXT
#define COL_DIM       THEME_DIM
#define COL_ACCENT    THEME_ACCENT
#define COL_GREEN     THEME_GREEN
#define COL_AMBER     THEME_AMBER
#define COL_RED       THEME_RED
#define COL_BAR_BG    THEME_BAR_BG

// ---- Layout constants for 368x448 (Waveshare 1.8" AMOLED) ----
#define SCR_W         368
#define SCR_H         448
#define MARGIN        12    // tighter margin — display has less screen real-estate
#define TITLE_Y       30
#define CONTENT_Y     100
#define CONTENT_W     (SCR_W - 2 * MARGIN)   // 344

// ---- Usage screen widgets ----
static lv_obj_t* usage_container;
static lv_obj_t* lbl_title;
static lv_obj_t* bar_session;
static lv_obj_t* lbl_session_pct;
static lv_obj_t* lbl_session_label;
static lv_obj_t* lbl_session_reset;
static lv_obj_t* bar_weekly;
static lv_obj_t* lbl_weekly_pct;
static lv_obj_t* lbl_weekly_label;
static lv_obj_t* lbl_weekly_reset;
static lv_obj_t* lbl_anim;

// ---- Bluetooth screen widgets ----
static lv_obj_t* ble_container;
static lv_obj_t* lbl_ble_status;
static lv_obj_t* lbl_ble_device;
static lv_obj_t* lbl_ble_mac;

// ---- Attention overlay widgets ----
static lv_obj_t* attn_container;
static lv_obj_t* lbl_attn_title;
static lv_obj_t* lbl_attn_msg;
static char      last_attn_msg[96] = {0};

// ---- Sessions screen widgets ----
static lv_obj_t* sess_container;
static lv_obj_t* sess_empty_lbl;
static lv_obj_t* sess_rows[MAX_SESSIONS];
static lv_obj_t* sess_row_dot[MAX_SESSIONS];
static lv_obj_t* sess_row_proj[MAX_SESSIONS];
static lv_obj_t* sess_row_state[MAX_SESSIONS];
static lv_obj_t* sess_row_msg[MAX_SESSIONS];

// ---- Battery indicator (shared, on top) ----
static lv_obj_t* battery_img;
static lv_obj_t* logo_img;
static lv_image_dsc_t battery_dscs[5];  // empty, low, medium, full, charging

// ---- Shared ----
static lv_image_dsc_t logo_dsc;
static screen_t current_screen = SCREEN_USAGE;

// Animation state
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;
static uint32_t anim_msg_start = 0;
#define ANIM_MSG_MS     4000

static const char* const spinner_frames[] = {
    "\xC2\xB7", "\xE2\x9C\xBB", "\xE2\x9C\xBD",
    "\xE2\x9C\xB6", "\xE2\x9C\xB3", "\xE2\x9C\xA2",
};
#define SPINNER_COUNT 6
#define SPINNER_PHASES (2 * (SPINNER_COUNT - 1))  // 10: ping-pong 0..5..0

// Per-frame hold time. Modeled on Claude Code's spinner (Cavalry triangle
// oscillator, range 0..5, period 5s) — turn-around frames (0 and 5) appear
// once per cycle, middle frames twice, so 0/5 read as held longer.
static const uint16_t spinner_ms[SPINNER_COUNT] = {
    260, 130, 130, 130, 130, 260,
};

static const char* const anim_messages[] = {
    "Accomplishing", "Elucidating", "Perusing",
    "Actioning", "Enchanting", "Philosophising",
    "Actualizing", "Envisioning", "Pondering",
    "Baking", "Finagling", "Pontificating",
    "Booping", "Flibbertigibbeting", "Processing",
    "Brewing", "Forging", "Puttering",
    "Calculating", "Forming", "Puzzling",
    "Cerebrating", "Frolicking", "Reticulating",
    "Channelling", "Generating", "Ruminating",
    "Churning", "Germinating", "Scheming",
    "Clauding", "Hatching", "Schlepping",
    "Coalescing", "Herding", "Shimmying",
    "Cogitating", "Honking", "Shucking",
    "Combobulating", "Hustling", "Simmering",
    "Computing", "Ideating", "Smooshing",
    "Concocting", "Imagining", "Spelunking",
    "Conjuring", "Incubating", "Spinning",
    "Considering", "Inferring", "Stewing",
    "Contemplating", "Jiving", "Sussing",
    "Cooking", "Manifesting", "Synthesizing",
    "Crafting", "Marinating", "Thinking",
    "Creating", "Meandering", "Tinkering",
    "Crunching", "Moseying", "Transmuting",
    "Deciphering", "Mulling", "Unfurling",
    "Deliberating", "Mustering", "Unravelling",
    "Determining", "Musing", "Vibing",
    "Discombobulating", "Noodling", "Wandering",
    "Divining", "Percolating", "Whirring",
    "Doing", "Wibbling",
    "Effecting", "Wizarding",
    "Working", "Wrangling",
};
#define ANIM_MSG_COUNT (sizeof(anim_messages) / sizeof(anim_messages[0]))

static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

static void format_reset_time(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "Resets in %dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "Resets in %dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "Resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

// Forward decls — callbacks defined near ui_show_screen below
static void global_click_cb(lv_event_t* e);
static void global_long_press_cb(lv_event_t* e);
static void ble_reset_click_cb(lv_event_t* e);

static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, 16, 0);
    lv_obj_set_style_pad_right(panel, 16, 0);
    lv_obj_set_style_pad_top(panel, 12, 0);
    lv_obj_set_style_pad_bottom(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    // Bubble click events up to the screen / usage_container so a tap anywhere
    // on the panel fires the global click handler.
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    return panel;
}

static lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    return bar;
}

static void init_icon_dsc(lv_image_dsc_t* dsc, int w, int h, const uint16_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.stride = w * 2;
    dsc->data = (const uint8_t*)data;
    dsc->data_size = w * h * 2;
}

// RGB565A8: planar — w*h RGB565 pixels followed by w*h alpha bytes.
// Stride is RGB565-only (w*2); LVGL infers alpha plane location from header.
static void init_icon_dsc_rgb565a8(lv_image_dsc_t* dsc, int w, int h, const uint8_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data = data;
    dsc->data_size = w * h * 3;
}

static lv_obj_t* make_pill(lv_obj_t* parent, const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl, 18, 0);
    lv_obj_set_style_pad_right(lbl, 18, 0);
    lv_obj_set_style_pad_top(lbl, 6, 0);
    lv_obj_set_style_pad_bottom(lbl, 6, 0);
    return lbl;
}

// ---- Battery icon initialization ----
static void init_battery_icons(void) {
    init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_data);
    init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_W, ICON_BATTERY_LOW_H, icon_battery_low_data);
    init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_W, ICON_BATTERY_MEDIUM_H, icon_battery_medium_data);
    init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_W, ICON_BATTERY_FULL_H, icon_battery_full_data);
    init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_W, ICON_BATTERY_CHARGING_H, icon_battery_charging_data);
}

// ======== Usage Screen (368x448) ========

#define PANEL_H     150
#define PANEL_GAP   16

// One Session/Weekly panel: big % label, pill on the right, bar, reset label.
// Pill y=1: symmetric inside the panel — panel-outer-top → pill-top equals
// pill-bottom → bar-top (pill height 42 + panel pad_top 12 + bar y=56).
static void make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                             lv_obj_t** out_pct, lv_obj_t** out_pill,
                             lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, MARGIN, y, CONTENT_W, PANEL_H);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, &font_styrene_48, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, 56, CONTENT_W - 32, 24);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, &font_styrene_28, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, 94);
}

static void init_usage_screen(lv_obj_t* scr) {
    usage_container = lv_obj_create(scr);
    lv_obj_set_size(usage_container, SCR_W, SCR_H);
    lv_obj_set_pos(usage_container, 0, 0);
    lv_obj_set_style_bg_opa(usage_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_container, 0, 0);
    lv_obj_set_style_pad_all(usage_container, 0, 0);
    lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_SCROLLABLE);
    // No tap-to-splash anymore: tap and swipe are too easy to confuse. The
    // splash is reached via swipe-up gesture (handled at screen level).

    lbl_title = lv_label_create(usage_container);
    lv_label_set_text(lbl_title, "Usage");
    lv_obj_set_style_text_font(lbl_title, &font_tiempos_34, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    // Centered horizontally between logo (x≤92) and battery (x≥308), vertically
    // aligned with the logo's midline.
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, TITLE_Y + 18);

    make_usage_panel(usage_container, CONTENT_Y, "Current",
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset);
    make_usage_panel(usage_container, CONTENT_Y + PANEL_H + PANEL_GAP, "Weekly",
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset);

    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, &font_mono_18, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -8);
}

// ======== Settings Screen (368x448) ========
// Compact BLE status at top + volume slider + mute switch + reset-bonds button.

static lv_obj_t* volume_slider;
static lv_obj_t* volume_pct_lbl;
static lv_obj_t* mute_switch;

static void volume_changed_cb(lv_event_t* e) {
    int v = lv_slider_get_value(volume_slider);
    audio_set_volume(v);
    lv_label_set_text_fmt(volume_pct_lbl, "%d%%", v);
}

static void mute_changed_cb(lv_event_t* e) {
    bool on = lv_obj_has_state(mute_switch, LV_STATE_CHECKED);
    audio_set_muted(on);
}

static void init_bluetooth_screen(lv_obj_t* scr) {
    ble_container = lv_obj_create(scr);
    lv_obj_set_size(ble_container, SCR_W, SCR_H);
    lv_obj_set_pos(ble_container, 0, 0);
    lv_obj_set_style_bg_opa(ble_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ble_container, 0, 0);
    lv_obj_set_style_pad_all(ble_container, 0, 0);
    lv_obj_clear_flag(ble_container, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl_title = lv_label_create(ble_container);
    lv_label_set_text(lbl_title, "Settings");
    lv_obj_set_style_text_font(lbl_title, &font_tiempos_34, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, TITLE_Y + 18);

    // ---- Compact BLE status panel ----
    lv_obj_t* p_ble = make_panel(ble_container, MARGIN, CONTENT_Y, CONTENT_W, 90);

    static lv_image_dsc_t icon_bt_dsc;
    init_icon_dsc(&icon_bt_dsc, ICON_BLUETOOTH_W, ICON_BLUETOOTH_H, icon_bluetooth_data);
    lv_obj_t* bt_img = lv_image_create(p_ble);
    lv_image_set_src(bt_img, &icon_bt_dsc);
    lv_obj_set_pos(bt_img, 0, 4);

    lbl_ble_status = lv_label_create(p_ble);
    lv_label_set_text(lbl_ble_status, "Initializing...");
    lv_obj_set_style_text_font(lbl_ble_status, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_ble_status, COL_TEXT, 0);
    lv_obj_set_pos(lbl_ble_status, 48, 0);

    lbl_ble_device = lv_label_create(p_ble);
    lv_label_set_text(lbl_ble_device, "Device: ---");
    lv_obj_set_style_text_font(lbl_ble_device, &font_styrene_20, 0);
    lv_obj_set_style_text_color(lbl_ble_device, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_device, 48, 32);

    lbl_ble_mac = lv_label_create(p_ble);
    lv_label_set_text(lbl_ble_mac, "---");
    lv_obj_set_style_text_font(lbl_ble_mac, &font_styrene_20, 0);
    lv_obj_set_style_text_color(lbl_ble_mac, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_mac, 48, 54);

    // ---- Volume row ----
    int vol_y = CONTENT_Y + 90 + 14;
    lv_obj_t* p_vol = make_panel(ble_container, MARGIN, vol_y, CONTENT_W, 76);

    lv_obj_t* vol_lbl = lv_label_create(p_vol);
    lv_label_set_text(vol_lbl, "Volume");
    lv_obj_set_style_text_font(vol_lbl, &font_styrene_24, 0);
    lv_obj_set_style_text_color(vol_lbl, COL_TEXT, 0);
    lv_obj_set_pos(vol_lbl, 0, 0);

    volume_pct_lbl = lv_label_create(p_vol);
    lv_label_set_text_fmt(volume_pct_lbl, "%d%%", audio_get_volume());
    lv_obj_set_style_text_font(volume_pct_lbl, &font_styrene_24, 0);
    lv_obj_set_style_text_color(volume_pct_lbl, COL_DIM, 0);
    lv_obj_align(volume_pct_lbl, LV_ALIGN_TOP_RIGHT, 0, 0);

    volume_slider = lv_slider_create(p_vol);
    lv_obj_set_size(volume_slider, CONTENT_W - 32, 14);
    lv_obj_set_pos(volume_slider, 0, 36);
    lv_slider_set_range(volume_slider, 0, 100);
    lv_slider_set_value(volume_slider, audio_get_volume(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(volume_slider, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(volume_slider, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(volume_slider, COL_ACCENT, LV_PART_KNOB);
    lv_obj_add_event_cb(volume_slider, volume_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // ---- Mute row ----
    int mute_y = vol_y + 76 + 12;
    lv_obj_t* p_mute = make_panel(ble_container, MARGIN, mute_y, CONTENT_W, 60);

    lv_obj_t* mute_lbl = lv_label_create(p_mute);
    lv_label_set_text(mute_lbl, "Mute");
    lv_obj_set_style_text_font(mute_lbl, &font_styrene_24, 0);
    lv_obj_set_style_text_color(mute_lbl, COL_TEXT, 0);
    lv_obj_set_pos(mute_lbl, 0, 8);

    mute_switch = lv_switch_create(p_mute);
    lv_obj_set_size(mute_switch, 64, 32);
    lv_obj_align(mute_switch, LV_ALIGN_RIGHT_MID, 0, 0);
    if (audio_is_muted()) lv_obj_add_state(mute_switch, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(mute_switch, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(mute_switch, COL_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(mute_switch, mute_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // ---- Switch Computer row (compact) ----
    // Disconnects the currently-connected host so another laptop can pick
    // up the device. There is no separate "pairing" — once disconnected,
    // the device immediately re-advertises.
    int reset_y = mute_y + 60 + 12;
    lv_obj_t* reset_zone = lv_obj_create(ble_container);
    lv_obj_set_pos(reset_zone, MARGIN, reset_y);
    lv_obj_set_size(reset_zone, CONTENT_W, 56);
    lv_obj_set_style_bg_color(reset_zone, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(reset_zone, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(reset_zone, 8, 0);
    lv_obj_set_style_border_width(reset_zone, 0, 0);
    lv_obj_set_style_pad_column(reset_zone, 14, 0);
    lv_obj_set_flex_flow(reset_zone, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(reset_zone, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(reset_zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(reset_zone, ble_reset_click_cb, LV_EVENT_CLICKED, NULL);

    static lv_image_dsc_t icon_trash_dsc;
    init_icon_dsc(&icon_trash_dsc, ICON_TRASH2_W, ICON_TRASH2_H, icon_trash2_data);
    lv_obj_t* trash_img = lv_image_create(reset_zone);
    lv_image_set_src(trash_img, &icon_trash_dsc);

    lv_obj_t* reset_lbl = lv_label_create(reset_zone);
    lv_label_set_text(reset_lbl, "Switch Computer");
    lv_obj_set_style_text_font(reset_lbl, &font_styrene_24, 0);
    lv_obj_set_style_text_color(reset_lbl, COL_DIM, 0);

    // Start hidden
    lv_obj_add_flag(ble_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== Attention overlay ========
//
// Shown when Claude Code is waiting on the user (permission prompt, idle
// notification). Triggered by `at` field in the BLE payload — non-empty
// → show, empty → hide and restore previous screen.

static void attn_dismiss_cb(lv_event_t* e) {
    (void)e;
    // Tap-to-dismiss: clear the message and restore previous screen.
    ui_set_attn(NULL);
}

static void init_attn_screen(lv_obj_t* scr) {
    attn_container = lv_obj_create(scr);
    lv_obj_set_size(attn_container, SCR_W, SCR_H);
    lv_obj_set_pos(attn_container, 0, 0);
    lv_obj_set_style_bg_color(attn_container, COL_ACCENT, 0);
    lv_obj_set_style_bg_opa(attn_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(attn_container, 0, 0);
    lv_obj_set_style_pad_all(attn_container, MARGIN, 0);
    lv_obj_clear_flag(attn_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(attn_container, attn_dismiss_cb, LV_EVENT_CLICKED, NULL);

    lbl_attn_title = lv_label_create(attn_container);
    lv_label_set_text(lbl_attn_title, "CLAUDE");
    lv_obj_set_style_text_font(lbl_attn_title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(lbl_attn_title, COL_BG, 0);
    lv_obj_align(lbl_attn_title, LV_ALIGN_TOP_MID, 0, 80);

    lv_obj_t* lbl_subtitle = lv_label_create(attn_container);
    lv_label_set_text(lbl_subtitle, "is waiting");
    lv_obj_set_style_text_font(lbl_subtitle, &font_styrene_48, 0);
    lv_obj_set_style_text_color(lbl_subtitle, COL_BG, 0);
    lv_obj_align(lbl_subtitle, LV_ALIGN_TOP_MID, 0, 160);

    lbl_attn_msg = lv_label_create(attn_container);
    lv_label_set_text(lbl_attn_msg, "");
    lv_obj_set_style_text_font(lbl_attn_msg, &font_styrene_24, 0);
    lv_obj_set_style_text_color(lbl_attn_msg, COL_BG, 0);
    lv_obj_set_style_text_align(lbl_attn_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lbl_attn_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl_attn_msg, SCR_W - 2 * MARGIN);
    lv_obj_align(lbl_attn_msg, LV_ALIGN_BOTTOM_MID, 0, -60);

    lv_obj_add_flag(attn_container, LV_OBJ_FLAG_HIDDEN);
}

// Tracks the screen the user was on before attention took over.
static screen_t pre_attn_screen = SCREEN_USAGE;
static bool     attn_visible    = false;

void ui_set_attn(const char* msg) {
    bool want_visible = (msg != NULL && msg[0] != '\0');

    if (want_visible) {
        // Don't blow away pre_attn_screen on repeated calls while already up.
        if (!attn_visible) {
            pre_attn_screen = current_screen;
            attn_visible = true;
        }
        lv_label_set_text(lbl_attn_msg, msg);
        strlcpy(last_attn_msg, msg, sizeof(last_attn_msg));
        lv_obj_clear_flag(attn_container, LV_OBJ_FLAG_HIDDEN);
        // Hide everything else so the overlay is the entire view.
        lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ble_container, LV_OBJ_FLAG_HIDDEN);
        splash_hide();
        if (logo_img) lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        if (battery_img) lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(attn_container);
    } else if (attn_visible) {
        attn_visible = false;
        last_attn_msg[0] = '\0';
        lv_obj_add_flag(attn_container, LV_OBJ_FLAG_HIDDEN);
        ui_show_screen(pre_attn_screen);
    }
}

// ======== Sessions screen ========
//
// Shows up to MAX_SESSIONS rows, one per active CC session: a colored status
// dot, the project name, the current state label, and (for waiting sessions)
// the notification message.

#define SESS_ROW_H        56
#define SESS_ROW_GAP      6
#define SESS_DOT_DIAMETER 16

static lv_color_t sess_state_color(sess_state_t st) {
    switch (st) {
    case SESS_WAITING: return COL_RED;
    case SESS_WORKING: return COL_AMBER;
    case SESS_IDLE:    return COL_GREEN;
    }
    return COL_DIM;
}

static const char* sess_state_label(sess_state_t st) {
    switch (st) {
    case SESS_WAITING: return "waiting";
    case SESS_WORKING: return "working";
    case SESS_IDLE:    return "idle";
    }
    return "?";
}

static void init_sessions_screen(lv_obj_t* scr) {
    sess_container = lv_obj_create(scr);
    lv_obj_set_size(sess_container, SCR_W, SCR_H);
    lv_obj_set_pos(sess_container, 0, 0);
    lv_obj_set_style_bg_opa(sess_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sess_container, 0, 0);
    lv_obj_set_style_pad_all(sess_container, 0, 0);
    lv_obj_clear_flag(sess_container, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl_title = lv_label_create(sess_container);
    lv_label_set_text(lbl_title, "Sessions");
    lv_obj_set_style_text_font(lbl_title, &font_tiempos_34, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, TITLE_Y + 18);

    sess_empty_lbl = lv_label_create(sess_container);
    lv_label_set_text(sess_empty_lbl, "No active sessions");
    lv_obj_set_style_text_font(sess_empty_lbl, &font_styrene_24, 0);
    lv_obj_set_style_text_color(sess_empty_lbl, COL_DIM, 0);
    lv_obj_align(sess_empty_lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(sess_empty_lbl, LV_OBJ_FLAG_HIDDEN);

    int row_y = CONTENT_Y;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        lv_obj_t* row = make_panel(sess_container, MARGIN, row_y, CONTENT_W, SESS_ROW_H);

        sess_row_dot[i] = lv_obj_create(row);
        lv_obj_set_size(sess_row_dot[i], SESS_DOT_DIAMETER, SESS_DOT_DIAMETER);
        lv_obj_set_pos(sess_row_dot[i], 0, 4);
        lv_obj_set_style_radius(sess_row_dot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(sess_row_dot[i], 0, 0);
        lv_obj_set_style_bg_opa(sess_row_dot[i], LV_OPA_COVER, 0);
        lv_obj_clear_flag(sess_row_dot[i], LV_OBJ_FLAG_SCROLLABLE);

        sess_row_proj[i] = lv_label_create(row);
        lv_obj_set_style_text_font(sess_row_proj[i], &font_styrene_24, 0);
        lv_obj_set_style_text_color(sess_row_proj[i], COL_TEXT, 0);
        lv_obj_set_pos(sess_row_proj[i], SESS_DOT_DIAMETER + 10, 0);

        sess_row_state[i] = lv_label_create(row);
        lv_obj_set_style_text_font(sess_row_state[i], &font_styrene_20, 0);
        lv_obj_set_style_text_color(sess_row_state[i], COL_DIM, 0);
        lv_obj_align(sess_row_state[i], LV_ALIGN_TOP_RIGHT, 0, 4);

        sess_row_msg[i] = lv_label_create(row);
        lv_obj_set_style_text_font(sess_row_msg[i], &font_styrene_14, 0);
        lv_obj_set_style_text_color(sess_row_msg[i], COL_DIM, 0);
        lv_label_set_long_mode(sess_row_msg[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(sess_row_msg[i], CONTENT_W - 2 * 16);
        lv_obj_set_pos(sess_row_msg[i], SESS_DOT_DIAMETER + 10, 26);

        sess_rows[i] = row;
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        row_y += SESS_ROW_H + SESS_ROW_GAP;
    }

    lv_obj_add_flag(sess_container, LV_OBJ_FLAG_HIDDEN);
}

static void ui_update_sessions(const UsageData* d) {
    int n = d ? d->sessions_count : 0;
    if (n > MAX_SESSIONS) n = MAX_SESSIONS;

    if (sess_empty_lbl) {
        if (n == 0) lv_obj_clear_flag(sess_empty_lbl, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_add_flag(sess_empty_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (i < n) {
            const SessionInfo* s = &d->sessions[i];
            lv_obj_set_style_bg_color(sess_row_dot[i], sess_state_color(s->state), 0);
            lv_label_set_text(sess_row_proj[i], s->proj);
            lv_label_set_text(sess_row_state[i], sess_state_label(s->state));
            lv_obj_set_style_text_color(sess_row_state[i],
                s->state == SESS_WAITING ? COL_RED : COL_DIM, 0);
            if (s->state == SESS_WAITING && s->msg[0]) {
                lv_label_set_text(sess_row_msg[i], s->msg);
                lv_obj_clear_flag(sess_row_msg[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(sess_row_msg[i], LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_clear_flag(sess_rows[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(sess_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ======== Swipe gesture handling ========
//
// Left/right swipes cycle through Usage → Sessions → Bluetooth → back to
// Usage. Splash is reached only via the existing tap-anywhere path.

static void screen_gesture_cb(lv_event_t* e) {
    (void)e;
    lv_indev_t* indev = lv_indev_active();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    screen_t s = ui_get_current_screen();

    // Swipe-up from any non-splash → splash.
    // Swipe in any direction on splash → dismiss back to previous screen.
    if (s == SCREEN_SPLASH) {
        if (dir == LV_DIR_TOP || dir == LV_DIR_BOTTOM ||
            dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
            ui_toggle_splash();
        }
        return;
    }
    if (dir == LV_DIR_TOP) {
        ui_toggle_splash();
        return;
    }

    // Horizontal swipes cycle: USAGE ↔ SESSIONS ↔ BLUETOOTH.
    if (dir == LV_DIR_LEFT) {
        if      (s == SCREEN_USAGE)     ui_show_screen(SCREEN_SESSIONS);
        else if (s == SCREEN_SESSIONS)  ui_show_screen(SCREEN_BLUETOOTH);
    } else if (dir == LV_DIR_RIGHT) {
        if      (s == SCREEN_BLUETOOTH) ui_show_screen(SCREEN_SESSIONS);
        else if (s == SCREEN_SESSIONS)  ui_show_screen(SCREEN_USAGE);
    }
}

// ======== Public API ========

void ui_init(void) {
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Logo (shared, always visible, on top of all containers)
    // Logo is RGB565A8 (planar: w*h RGB565 then w*h alpha) so it composites
    // cleanly against whatever bg is behind it.
    init_icon_dsc_rgb565a8(&logo_dsc, LOGO_WIDTH, LOGO_HEIGHT, logo_data);

    // Initialize battery icon descriptors
    init_battery_icons();

    init_usage_screen(scr);
    init_sessions_screen(scr);
    init_bluetooth_screen(scr);
    splash_init(scr);
    init_attn_screen(scr);

    // Screen-level swipe gestures (left/right cycle through non-splash screens).
    lv_obj_add_event_cb(scr, screen_gesture_cb, LV_EVENT_GESTURE, NULL);

    // Splash is dismissed by a swipe-down (or swipe in any direction other
    // than the up that brought us in) — handled in the screen-level gesture
    // callback below.

    // Logo on top of all containers (inset for rounded corners)
    logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, &logo_dsc);
    lv_obj_set_pos(logo_img, MARGIN, TITLE_Y - 10);

    // Battery indicator on top of all containers (upper-right, inset)
    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    lv_obj_set_pos(battery_img, SCR_W - 48 - MARGIN, TITLE_Y);
}

void ui_update(const UsageData* data) {
    if (!data->valid) return;

    ui_update_sessions(data);

    int s_pct = (int)(data->session_pct + 0.5f);

    // Usage screen
    lv_label_set_text_fmt(lbl_session_pct, "%d%%", s_pct);
    lv_bar_set_value(bar_session, s_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_session, pct_color(data->session_pct), LV_PART_INDICATOR);

    char buf[48];
    format_reset_time(data->session_reset_mins, buf, sizeof(buf));
    lv_label_set_text(lbl_session_reset, buf);

    int w_pct = (int)(data->weekly_pct + 0.5f);
    lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", w_pct);
    lv_bar_set_value(bar_weekly, w_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_weekly, pct_color(data->weekly_pct), LV_PART_INDICATOR);

    format_reset_time(data->weekly_reset_mins, buf, sizeof(buf));
    lv_label_set_text(lbl_weekly_reset, buf);
}

void ui_tick_anim(void) {
    if (current_screen != SCREEN_USAGE) return;

    uint32_t now = lv_tick_get();

    if (now - anim_msg_start >= ANIM_MSG_MS) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_msg_start = now;
    }

    if (now - anim_last_ms >= spinner_ms[anim_spinner_idx]) {
        anim_last_ms = now;
        anim_phase = (anim_phase + 1) % SPINNER_PHASES;
        anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                        : (SPINNER_PHASES - anim_phase);

        static char buf[80];
        snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
                 spinner_frames[anim_spinner_idx],
                 anim_messages[anim_msg_idx]);
        lv_label_set_text(lbl_anim, buf);
    }
}

static screen_t prev_non_splash_screen = SCREEN_USAGE;
// Hide the battery indicator on the splash screen — the icon is visually
// noisy over the pixel-art creature animations.
static void apply_battery_visibility(void) {
    if (!battery_img) return;
    if (current_screen == SCREEN_SPLASH) lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    else                                  lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
}

// LVGL handles click debouncing internally. Screen-level handler fires when
// no child consumed the event (children only consume if they have their own
// event callback, e.g. the Reset Bluetooth zone). On BT screen we skip the
// splash toggle so only the reset zone is interactive there.
static void global_click_cb(lv_event_t* e) {
    (void)e;
    if (ui_get_current_screen() == SCREEN_BLUETOOTH) return;
    ui_toggle_splash();
}

// Long-press on any non-reset zone cycles screens. This gives a touch-only
// path to the Bluetooth screen on boards where the AXP2101 PWR button isn't
// physically exposed (e.g. the Waveshare 1.8").
static void global_long_press_cb(lv_event_t* e) {
    (void)e;
    screen_t s = ui_get_current_screen();
    if (s == SCREEN_SPLASH) return;
    ui_cycle_screen();
}

static void ble_reset_click_cb(lv_event_t* e) {
    (void)e;
    // Button is labelled "Switch Computer" — drop the current peer so
    // another laptop's daemon can connect. With bonding disabled there
    // are no bonds to clear, so we just release.
    ble_release_host();
}

void ui_show_screen(screen_t screen) {
    lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(sess_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ble_container, LV_OBJ_FLAG_HIDDEN);
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:     splash_show(); break;
    case SCREEN_USAGE:      lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_SESSIONS:   lv_obj_clear_flag(sess_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_BLUETOOTH:  lv_obj_clear_flag(ble_container, LV_OBJ_FLAG_HIDDEN); break;
    default: break;
    }

    // Hide the logo overlay on the splash screen so the animation has a clean canvas
    if (logo_img) {
        if (screen == SCREEN_SPLASH) lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        else                          lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
    }

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
    apply_battery_visibility();
}

void ui_cycle_screen(void) {
    screen_t next = (current_screen == SCREEN_USAGE) ? SCREEN_BLUETOOTH : SCREEN_USAGE;
    ui_show_screen(next);
}

void ui_toggle_splash(void) {
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    switch (state) {
    case BLE_STATE_CONNECTED:
        lv_label_set_text(lbl_ble_status, "Connected");
        lv_obj_set_style_text_color(lbl_ble_status, COL_GREEN, 0);
        break;
    case BLE_STATE_ADVERTISING:
        lv_label_set_text(lbl_ble_status, "Advertising...");
        lv_obj_set_style_text_color(lbl_ble_status, COL_AMBER, 0);
        break;
    case BLE_STATE_DISCONNECTED:
        lv_label_set_text(lbl_ble_status, "Disconnected");
        lv_obj_set_style_text_color(lbl_ble_status, COL_RED, 0);
        break;
    default:
        lv_label_set_text(lbl_ble_status, "Initializing...");
        lv_obj_set_style_text_color(lbl_ble_status, COL_DIM, 0);
        break;
    }

    if (name) {
        static char nbuf[48];
        snprintf(nbuf, sizeof(nbuf), "Device: %s", name);
        lv_label_set_text(lbl_ble_device, nbuf);
    }
    if (mac) {
        static char mbuf[48];
        snprintf(mbuf, sizeof(mbuf), "Address: %s", mac);
        lv_label_set_text(lbl_ble_mac, mbuf);
    }
}

void ui_update_battery(int percent, bool charging) {
    int idx;
    if (charging) {
        idx = 4;  // charging icon
    } else if (percent < 0) {
        idx = 0;  // no battery / unknown
    } else if (percent <= 10) {
        idx = 0;  // empty
    } else if (percent <= 35) {
        idx = 1;  // low
    } else if (percent <= 75) {
        idx = 2;  // medium
    } else {
        idx = 3;  // full
    }
    lv_image_set_src(battery_img, &battery_dscs[idx]);
    apply_battery_visibility();
}
