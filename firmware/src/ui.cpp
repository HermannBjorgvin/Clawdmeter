#include "ui.h"
#include "splash.h"
#include <lvgl.h>
#include "logo.h"
#include "icons.h"
#include "hal/board_caps.h"

// Custom fonts (scaled for 314 PPI, ~1.9x from original 165 PPI)
LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_mono_32);
LV_FONT_DECLARE(font_styrene_16);

// AMOLED-1.8 (368 wide) needs smaller fonts on the Bluetooth screen so the
// MAC address and credit lines don't overflow horizontally.
#define BT_TITLE_FONT     font_tiempos_56
#define BT_STATUS_FONT    font_styrene_48
#define BT_DEVICE_FONT    font_styrene_28
#define BT_CREDIT_1_FONT  font_styrene_24
#define BT_CREDIT_2_FONT  font_styrene_20

// Activity screen font + spacing budget. Portrait 368x448 has less
// vertical room, but the previous font scale (12-16pt) was unreadable
// at arm's length — bumped one or two steps with a tighter list region.
// Activity is the only screen that uses its own title baseline (everything
// else aligns to the shared TITLE_Y=30). We push it ~15px lower so the
// title doesn't crowd the rounded display corners and battery icon.
// Per upstream review feedback (HermannBjorgvin/Clawdmeter#22): bigger
// fonts across the board and the footer dropped, giving the panel more
// vertical room.
#define ACT_TITLE_FONT     font_styrene_28
#define ACT_MODEL_FONT     font_styrene_24
#define ACT_PROMPT_FONT    font_styrene_24
#define ACT_ACTIVE_FONT    font_styrene_28
#define ACT_PROGRESS_FONT  font_styrene_24
#define ACT_TODO_FONT      font_styrene_24
#define ACT_TODO_ROW_H     34
#define ACT_TITLE_Y        50
#define ACT_TITLE_H        36
#define ACT_MODEL_Y        94
#define ACT_PROMPT_Y       130
#define ACT_PROMPT_H       34
#define ACT_ACTIVE_Y       178
#define ACT_ACTIVE_H       64
#define ACT_PANEL_Y        252
#define ACT_PANEL_H        222
#define ACT_COUNTER_RIGHT  76

// Cap visible todo rows so we can size the panel deterministically.
#define ACT_TODO_WINDOW    5

// Layout values computed from the active board's geometry. Populated once
// in ui_init() and treated as const for the rest of the program. Adding a
// new display size means extending compute_layout() with another
// breakpoint — never editing the screen-builder functions below.
struct Layout {
    int16_t scr_w, scr_h;
    int16_t margin;
    int16_t title_y;
    int16_t content_y;
    int16_t content_w;

    // Usage screen
    int16_t usage_panel_h;
    int16_t usage_panel_gap;
    int16_t usage_bar_y;
    int16_t usage_reset_y;

    // Bluetooth screen
    int16_t bt_info_panel_h;
    int16_t bt_reset_zone_h;
    const lv_font_t* bt_title_font;
    const lv_font_t* bt_status_font;
    const lv_font_t* bt_device_font;
    const lv_font_t* bt_credit_1_font;
    const lv_font_t* bt_credit_2_font;
};
static Layout L = {};

// Pick layout values from the active board's pixel dimensions. The two
// existing boards happen to land on the two breakpoints below; new ports
// inherit the closer one — visually OK, may need a polish pass for
// pixel-perfect alignment but never blocks the port from booting.
static void compute_layout(const BoardCaps& c) {
    L.scr_w = c.width;
    L.scr_h = c.height;
    L.margin = 20;
    L.title_y = 30;

    if (c.height >= 460) {
        // Large layout — tuned for 480x480 (AMOLED-2.16).
        L.content_y = 100;
        L.usage_panel_h = 150;
        L.usage_panel_gap = 16;
        L.usage_bar_y = 56;
        L.usage_reset_y = 94;
        L.bt_info_panel_h = 160;
        L.bt_reset_zone_h = 110;
        L.bt_title_font    = &font_tiempos_56;
        L.bt_status_font   = &font_styrene_48;
        L.bt_device_font   = &font_styrene_28;
        L.bt_credit_1_font = &font_styrene_24;
        L.bt_credit_2_font = &font_styrene_20;
    } else {
        // Compact layout — tuned for 368x448 (AMOLED-1.8).
        L.content_y = 85;
        L.usage_panel_h = 130;
        L.usage_panel_gap = 12;
        L.usage_bar_y = 48;
        L.usage_reset_y = 78;
        L.bt_info_panel_h = 140;
        L.bt_reset_zone_h = 90;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_28;
        L.bt_device_font   = &font_styrene_20;
        L.bt_credit_1_font = &font_styrene_16;
        L.bt_credit_2_font = &font_styrene_14;
    }

    L.content_w = L.scr_w - 2 * L.margin;
}

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

// ---- Activity screen widgets ----
static lv_obj_t* activity_container;
static lv_obj_t* lbl_act_title;          // project name (line 1, accent text color)
static lv_obj_t* lbl_act_model;          // model name (line 2, dim subtitle)
static lv_obj_t* lbl_act_counter;        // "1/3", colored by phase (green=running, dim=idle)
static lv_obj_t* lbl_act_prompt;         // dim, last user prompt (1 line ellipsized)
static lv_obj_t* lbl_act_in_progress;    // ">> Reworking UI layout"
static lv_obj_t* act_todo_panel;         // rounded card wrapping progress + list
static lv_obj_t* lbl_act_progress;       // "5/12 done" — header inside the panel
static lv_obj_t* act_list;               // scrollable flex container of todo rows
static ActivityData cached_activity = {};
static uint8_t current_session_idx = 0;

// ---- Bluetooth screen widgets ----
static lv_obj_t* ble_container;
static lv_obj_t* lbl_ble_status;
static lv_obj_t* lbl_ble_device;
static lv_obj_t* lbl_ble_mac;

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
static void ble_reset_click_cb(lv_event_t* e);
static void activity_gesture_cb(lv_event_t* e);
static void render_activity(void);
static void apply_default_screen_state(void);

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

static void init_battery_icons(void) {
    init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_data);
    init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_W, ICON_BATTERY_LOW_H, icon_battery_low_data);
    init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_W, ICON_BATTERY_MEDIUM_H, icon_battery_medium_data);
    init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_W, ICON_BATTERY_FULL_H, icon_battery_full_data);
    init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_W, ICON_BATTERY_CHARGING_H, icon_battery_charging_data);
}

// ======== Usage Screen ========

// One Session/Weekly panel: big % label, pill on the right, bar, reset label.
// Pill y=1: symmetric inside the panel — panel-outer-top → pill-top equals
// pill-bottom → bar-top.
static void make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                             lv_obj_t** out_pct, lv_obj_t** out_pill,
                             lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, L.margin, y, L.content_w, L.usage_panel_h);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, &font_styrene_48, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, L.usage_bar_y, L.content_w - 32, 24);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, &font_styrene_28, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, L.usage_reset_y);
}

static void init_usage_screen(lv_obj_t* scr) {
    usage_container = lv_obj_create(scr);
    lv_obj_set_size(usage_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_container, 0, 0);
    lv_obj_set_style_bg_opa(usage_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_container, 0, 0);
    lv_obj_set_style_pad_all(usage_container, 0, 0);
    lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(usage_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lbl_title = lv_label_create(usage_container);
    lv_label_set_text(lbl_title, "Usage");
    lv_obj_set_style_text_font(lbl_title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 16, L.title_y);

    make_usage_panel(usage_container, L.content_y, "Current",
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset);
    make_usage_panel(usage_container,
                     L.content_y + L.usage_panel_h + L.usage_panel_gap, "Weekly",
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset);

    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, &font_mono_32, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -15);
}

// ======== Bluetooth Screen ========

static void init_bluetooth_screen(lv_obj_t* scr) {
    ble_container = lv_obj_create(scr);
    lv_obj_set_size(ble_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(ble_container, 0, 0);
    lv_obj_set_style_bg_opa(ble_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ble_container, 0, 0);
    lv_obj_set_style_pad_all(ble_container, 0, 0);
    lv_obj_clear_flag(ble_container, LV_OBJ_FLAG_SCROLLABLE);
    // Tap on BT background (anywhere outside the reset zone) cycles to the
    // next screen. The reset zone's own handler consumes its taps first.
    lv_obj_add_event_cb(ble_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* lbl_ble_title = lv_label_create(ble_container);
    lv_label_set_text(lbl_ble_title, "Bluetooth");
    lv_obj_set_style_text_font(lbl_ble_title, L.bt_title_font, 0);
    lv_obj_set_style_text_color(lbl_ble_title, COL_TEXT, 0);
    lv_obj_align(lbl_ble_title, LV_ALIGN_TOP_MID, 16, L.title_y);

    // Info panel
    lv_obj_t* p_info = make_panel(ble_container, L.margin, L.content_y,
                                  L.content_w, L.bt_info_panel_h);

    static lv_image_dsc_t icon_bt_dsc;
    init_icon_dsc(&icon_bt_dsc, ICON_BLUETOOTH_W, ICON_BLUETOOTH_H, icon_bluetooth_data);

    lv_obj_t* bt_img = lv_image_create(p_info);
    lv_image_set_src(bt_img, &icon_bt_dsc);
    lv_obj_set_pos(bt_img, 0, 0);

    lbl_ble_status = lv_label_create(p_info);
    lv_label_set_text(lbl_ble_status, "Initializing...");
    lv_obj_set_style_text_font(lbl_ble_status, L.bt_status_font, 0);
    lv_obj_set_style_text_color(lbl_ble_status, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_status, 56, 2);

    lbl_ble_device = lv_label_create(p_info);
    lv_label_set_text(lbl_ble_device, "Device: ---");
    lv_obj_set_style_text_font(lbl_ble_device, L.bt_device_font, 0);
    lv_obj_set_style_text_color(lbl_ble_device, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_device, 0, 64);

    lbl_ble_mac = lv_label_create(p_info);
    lv_label_set_text(lbl_ble_mac, "Address: ---");
    lv_obj_set_style_text_font(lbl_ble_mac, L.bt_device_font, 0);
    lv_obj_set_style_text_color(lbl_ble_mac, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_mac, 0, 100);

    // Reset Bluetooth tap zone with trash icon
    int reset_y = L.content_y + L.bt_info_panel_h + 16;
    lv_obj_t* reset_zone = lv_obj_create(ble_container);
    lv_obj_set_pos(reset_zone, L.margin, reset_y);
    lv_obj_set_size(reset_zone, L.content_w, L.bt_reset_zone_h);
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
    lv_label_set_text(reset_lbl, "Reset Bluetooth");
    lv_obj_set_style_text_font(reset_lbl, L.bt_device_font, 0);
    lv_obj_set_style_text_color(reset_lbl, COL_DIM, 0);

    lv_obj_t* lbl_credit = lv_label_create(ble_container);
    lv_label_set_text(lbl_credit, "Built by @hermannbjorgvin");
    lv_obj_set_style_text_font(lbl_credit, L.bt_credit_1_font, 0);
    lv_obj_set_style_text_color(lbl_credit, COL_DIM, 0);
    lv_obj_align(lbl_credit, LV_ALIGN_BOTTOM_MID, 0, -46);

    lv_obj_t* lbl_credit2 = lv_label_create(ble_container);
    lv_label_set_text(lbl_credit2, "Clawd animation by @amaanbuilds");
    lv_obj_set_style_text_font(lbl_credit2, L.bt_credit_2_font, 0);
    lv_obj_set_style_text_color(lbl_credit2, COL_DIM, 0);
    lv_obj_align(lbl_credit2, LV_ALIGN_BOTTOM_MID, 0, -20);

    lv_obj_add_flag(ble_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== Activity Screen ========

static void init_activity_screen(lv_obj_t* scr) {
    activity_container = lv_obj_create(scr);
    lv_obj_set_size(activity_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(activity_container, 0, 0);
    lv_obj_set_style_bg_opa(activity_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(activity_container, 0, 0);
    lv_obj_set_style_pad_all(activity_container, 0, 0);
    lv_obj_clear_flag(activity_container, LV_OBJ_FLAG_SCROLLABLE);
    // Tap to toggle splash (consistent with Usage screen).
    lv_obj_add_event_cb(activity_container, global_click_cb, LV_EVENT_CLICKED, NULL);
    // Swipe left/right to cycle sessions. LV_EVENT_GESTURE fires on the
    // indev's last-pressed object, NOT on the screen — so attach to the
    // top-level screen and gate by current_screen inside the callback.
    // (We also leave a copy on activity_container for the rare case where
    // the touch happens to start on the bare container background.)
    lv_obj_add_event_cb(lv_screen_active(),
                        activity_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(activity_container,
                        activity_gesture_cb, LV_EVENT_GESTURE, NULL);

    // Title row — left: "project | model", right: "N/M" page counter
    // colored by phase (green = running, dim = idle). Split into two
    // labels because LVGL 9 doesn't have a clean way to recolor a single
    // label across slices.
    lbl_act_counter = lv_label_create(activity_container);
    lv_label_set_text(lbl_act_counter, "");
    lv_obj_set_style_text_font(lbl_act_counter, &ACT_TITLE_FONT, 0);
    lv_obj_set_style_text_color(lbl_act_counter, COL_DIM, 0);
    // Shifted left of the battery icon (which sits at L.scr_w - 48 - L.margin).
    lv_obj_align(lbl_act_counter, LV_ALIGN_TOP_RIGHT, -ACT_COUNTER_RIGHT, ACT_TITLE_Y);

    lbl_act_title = lv_label_create(activity_container);
    lv_label_set_text(lbl_act_title, "");
    lv_obj_set_style_text_font(lbl_act_title, &ACT_TITLE_FONT, 0);
    lv_obj_set_style_text_color(lbl_act_title, COL_TEXT, 0);
    lv_obj_set_pos(lbl_act_title, L.margin, ACT_TITLE_Y);
    // Reserve right margin for: counter (~50px worst case "99/99") +
    // its own ACT_COUNTER_RIGHT pad + a small gap. Fixed height needed
    // alongside LV_LABEL_LONG_DOT or the label wraps instead of clipping.
    lv_obj_set_size(lbl_act_title,
                    L.content_w - (ACT_COUNTER_RIGHT - L.margin) - 50, ACT_TITLE_H);
    // Marquee-scroll long project|model strings so we keep the full name
    // visible without truncating.
    lv_label_set_long_mode(lbl_act_title, LV_LABEL_LONG_SCROLL_CIRCULAR);

    // Model on its own line (subtitle under project name).
    lbl_act_model = lv_label_create(activity_container);
    lv_label_set_text(lbl_act_model, "");
    lv_obj_set_style_text_font(lbl_act_model, &ACT_MODEL_FONT, 0);
    lv_obj_set_style_text_color(lbl_act_model, COL_DIM, 0);
    lv_obj_set_pos(lbl_act_model, L.margin, ACT_MODEL_Y);
    lv_obj_set_size(lbl_act_model, L.content_w, 20);
    lv_label_set_long_mode(lbl_act_model, LV_LABEL_LONG_DOT);

    // Last user prompt — dim, single ellipsized line under title. Hidden
    // when the daemon hasn't reported a prompt for this session yet.
    // LV_LABEL_LONG_DOT requires a fixed size or it wraps instead of clipping.
    lbl_act_prompt = lv_label_create(activity_container);
    lv_label_set_text(lbl_act_prompt, "");
    lv_obj_set_style_text_font(lbl_act_prompt, &ACT_PROMPT_FONT, 0);
    lv_obj_set_style_text_color(lbl_act_prompt, COL_DIM, 0);
    lv_obj_set_pos(lbl_act_prompt, L.margin, ACT_PROMPT_Y);
    lv_obj_set_size(lbl_act_prompt, L.content_w, ACT_PROMPT_H);
    lv_label_set_long_mode(lbl_act_prompt, LV_LABEL_LONG_DOT);

    // Current in-progress activeForm — the headline of the screen.
    // Fixed height caps wrap to 2 lines so subsequent rows don't shift.
    lbl_act_in_progress = lv_label_create(activity_container);
    lv_label_set_text(lbl_act_in_progress, "");
    lv_obj_set_style_text_font(lbl_act_in_progress, &ACT_ACTIVE_FONT, 0);
    lv_obj_set_style_text_color(lbl_act_in_progress, COL_ACCENT, 0);
    lv_obj_set_pos(lbl_act_in_progress, L.margin, ACT_ACTIVE_Y);
    lv_obj_set_size(lbl_act_in_progress, L.content_w, ACT_ACTIVE_H);
    lv_label_set_long_mode(lbl_act_in_progress, LV_LABEL_LONG_DOT);

    // Rounded "card" wrapping the progress header + todo list, matching
    // the panel design language of the Usage and Bluetooth screens.
    act_todo_panel = make_panel(activity_container, L.margin, ACT_PANEL_Y,
                                L.content_w, ACT_PANEL_H);

    // Progress counter — "5/12 done" — sits at the top of the panel.
    lbl_act_progress = lv_label_create(act_todo_panel);
    lv_label_set_text(lbl_act_progress, "");
    lv_obj_set_style_text_font(lbl_act_progress, &ACT_PROGRESS_FONT, 0);
    lv_obj_set_style_text_color(lbl_act_progress, COL_TEXT, 0);
    lv_obj_set_pos(lbl_act_progress, 0, 0);

    // Scrollable todo list — flex column container, vertical scroll —
    // sits below the progress label inside the same panel.
    act_list = lv_obj_create(act_todo_panel);
    lv_obj_set_pos(act_list, 0, 32);  // 32px = progress font + small gap
    lv_obj_set_size(act_list, L.content_w - 32, ACT_PANEL_H - 32 - 24);
    lv_obj_set_style_bg_opa(act_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(act_list, 0, 0);
    lv_obj_set_style_pad_all(act_list, 0, 0);
    lv_obj_set_style_pad_row(act_list, 2, 0);
    lv_obj_set_flex_flow(act_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(act_list, LV_DIR_VER);
    // Don't intercept clicks that should bubble up to global_click_cb /
    // gestures on the container above.
    lv_obj_add_flag(act_list, LV_OBJ_FLAG_EVENT_BUBBLE);

    // No footer/placeholder: the title row + last_prompt + headline carry
    // enough liveness signal that "last active Xs ago" is redundant. The
    // empty-state lives on the splash animation (Activity ↔ Splash morph).

    lv_obj_add_flag(activity_container, LV_OBJ_FLAG_HIDDEN);
}

static const char* todo_prefix(todo_status_t s) {
    switch (s) {
    case TODO_COMPLETED:   return "[x] ";
    case TODO_IN_PROGRESS: return "[>] ";
    case TODO_PENDING:
    default:               return "[ ] ";
    }
}

static lv_color_t todo_color(todo_status_t s) {
    switch (s) {
    case TODO_COMPLETED:   return COL_GREEN;
    case TODO_IN_PROGRESS: return COL_ACCENT;
    case TODO_PENDING:
    default:               return COL_DIM;
    }
}

static void render_activity(void) {
    if (!activity_container) return;

    // Clear list children before re-populating (cheap; ≤10 items).
    lv_obj_clean(act_list);

    // No-sessions case: apply_default_screen_state() will have already
    // hidden activity_container and shown the splash animation. Just bail.
    const bool any = cached_activity.valid && cached_activity.session_count > 0;
    if (!any) return;
    lv_obj_clear_flag(lbl_act_title,       LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_act_model,       LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_act_counter,     LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_act_in_progress, LV_OBJ_FLAG_HIDDEN);

    if (current_session_idx >= cached_activity.session_count) current_session_idx = 0;
    const SessionData& s = cached_activity.sessions[current_session_idx];

    // Title — project name on line 1, model on line 2 (subtitle style).
    // Counter (right-aligned, phase-colored) is handled separately below.
    lv_label_set_text(lbl_act_title, s.project[0] ? s.project : "(unknown)");
    lv_label_set_text(lbl_act_model, s.model[0] ? s.model : "");

    // Counter — colored green when the agent is running, dim when idle.
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u/%u",
                 (unsigned)(current_session_idx + 1),
                 (unsigned)cached_activity.session_count);
        lv_label_set_text(lbl_act_counter, buf);
        lv_obj_set_style_text_color(lbl_act_counter,
            s.phase == PHASE_RUNNING ? COL_GREEN : COL_DIM, 0);
        // Re-align after text change so right-edge tracks the new width.
        lv_obj_align(lbl_act_counter, LV_ALIGN_TOP_RIGHT,
                     -ACT_COUNTER_RIGHT, ACT_TITLE_Y);
    }

    // Last user prompt — hide entirely when empty so the headline has the
    // visual weight users expect.
    if (s.last_prompt[0]) {
        lv_obj_clear_flag(lbl_act_prompt, LV_OBJ_FLAG_HIDDEN);
        char buf[USER_PROMPT_LEN + 4];
        snprintf(buf, sizeof(buf), "\"%s\"", s.last_prompt);
        lv_label_set_text(lbl_act_prompt, buf);
    } else {
        lv_obj_add_flag(lbl_act_prompt, LV_OBJ_FLAG_HIDDEN);
    }

    // Headline priority:
    //   1. an in-progress todo's activeForm
    //   2. otherwise current_tool ("Doing: Bash")
    //   3. otherwise phase == idle → "(idle)"
    //   4. otherwise "(no todos)"
    const TodoItem* in_progress = nullptr;
    int in_progress_idx = -1;
    int done = 0;
    for (uint8_t i = 0; i < s.todo_count; i++) {
        if (s.todos[i].status == TODO_COMPLETED) done++;
        if (s.todos[i].status == TODO_IN_PROGRESS && !in_progress) {
            in_progress = &s.todos[i];
            in_progress_idx = i;
        }
    }
    if (in_progress) {
        char buf[160];
        const char* text = in_progress->active_form[0] ? in_progress->active_form
                                                        : in_progress->content;
        snprintf(buf, sizeof(buf), ">>  %s", text);
        lv_label_set_text(lbl_act_in_progress, buf);
        lv_obj_set_style_text_color(lbl_act_in_progress, COL_ACCENT, 0);
    } else if (s.current_tool[0]) {
        // "Bash | <short summary>" when the hook captured tool_input.
        // Falls back to "Doing: <tool>" if no summary is available.
        char buf[128];
        if (s.current_tool_args[0]) {
            snprintf(buf, sizeof(buf), ">>  %s | %s",
                     s.current_tool, s.current_tool_args);
        } else {
            snprintf(buf, sizeof(buf), ">>  Doing: %s", s.current_tool);
        }
        lv_label_set_text(lbl_act_in_progress, buf);
        lv_obj_set_style_text_color(lbl_act_in_progress, COL_ACCENT, 0);
    } else if (s.phase == PHASE_IDLE) {
        lv_label_set_text(lbl_act_in_progress, "(idle)");
        lv_obj_set_style_text_color(lbl_act_in_progress, COL_DIM, 0);
    } else {
        lv_label_set_text(lbl_act_in_progress, "(no todos)");
        lv_obj_set_style_text_color(lbl_act_in_progress, COL_DIM, 0);
    }

    // Hide the whole todo panel when this session has no todos. An empty
    // framed card was visually noisy when the agent was running but
    // hadn't called TodoWrite yet (typical short tasks).
    if (s.todo_count == 0) {
        lv_obj_add_flag(act_todo_panel, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(act_todo_panel, LV_OBJ_FLAG_HIDDEN);

    // Progress counter.
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d/%u done", done, (unsigned)s.todo_count);
        lv_label_set_text(lbl_act_progress, buf);
    }

    // Windowed todo list — show at most ACT_TODO_WINDOW rows, centered on
    // the in-progress item when there is one (2 above, in-progress in the
    // middle, 2 below). Slides toward an edge if centering would overflow.
    // If there's no in-progress todo, just show the head of the list.
    int total = (int)s.todo_count;
    int win_start;
    if (total <= ACT_TODO_WINDOW) {
        win_start = 0;
    } else if (in_progress_idx < 0) {
        win_start = 0;
    } else {
        win_start = in_progress_idx - ACT_TODO_WINDOW / 2;
        if (win_start + ACT_TODO_WINDOW > total) win_start = total - ACT_TODO_WINDOW;
        if (win_start < 0) win_start = 0;
    }
    int win_end = win_start + ACT_TODO_WINDOW;
    if (win_end > total) win_end = total;

    for (int i = win_start; i < win_end; i++) {
        const TodoItem& t = s.todos[i];
        lv_obj_t* row = lv_label_create(act_list);
        char buf[TODO_CONTENT_LEN + 8];
        snprintf(buf, sizeof(buf), "%s%s", todo_prefix(t.status), t.content);
        lv_label_set_text(row, buf);
        lv_obj_set_style_text_font(row, &ACT_TODO_FONT, 0);
        lv_obj_set_style_text_color(row, todo_color(t.status), 0);
        lv_obj_set_size(row, L.content_w - 4, ACT_TODO_ROW_H);
        lv_label_set_long_mode(row, LV_LABEL_LONG_DOT);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
    }

}

static void activity_gesture_cb(lv_event_t* e) {
    (void)e;
    // Only act when the splash screen is currently morphed into Activity
    // (i.e. there's at least one session to swipe between). This handler
    // is registered on the screen root so it fires for every gesture
    // regardless of which screen is visible.
    if (ui_get_current_screen() != SCREEN_SPLASH) return;
    if (cached_activity.session_count <= 1) return;
    lv_indev_t* indev = lv_indev_active();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT) {
        current_session_idx = (current_session_idx + 1) % cached_activity.session_count;
    } else if (dir == LV_DIR_RIGHT) {
        current_session_idx = (current_session_idx + cached_activity.session_count - 1)
                              % cached_activity.session_count;
    } else {
        return;
    }
    render_activity();
    // Suppress further events from this swipe so render runs once per gesture.
    lv_indev_wait_release(indev);
}

// ======== Public API ========

void ui_init(void) {
    compute_layout(board_caps());

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    init_icon_dsc_rgb565a8(&logo_dsc, LOGO_WIDTH, LOGO_HEIGHT, logo_data);
    init_battery_icons();

    init_usage_screen(scr);
    init_activity_screen(scr);
    init_bluetooth_screen(scr);
    splash_init(scr);

    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, &logo_dsc);
    lv_obj_set_pos(logo_img, L.margin, L.title_y - 10);

    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    lv_obj_set_pos(battery_img, L.scr_w - 48 - L.margin, L.title_y);
}

void ui_update_activity(const ActivityData* data) {
    if (!data) return;
    cached_activity = *data;
    cached_activity.valid = true;
    if (cached_activity.session_count == 0) current_session_idx = 0;
    else if (current_session_idx >= cached_activity.session_count) current_session_idx = 0;
    render_activity();
    // If we're on the default (splash) screen, the morph state may have
    // changed from animation → activity widgets or vice versa.
    apply_default_screen_state();
}

void ui_update(const UsageData* data) {
    if (!data->valid) return;

    int s_pct = (int)(data->session_pct + 0.5f);

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
static void apply_battery_visibility(void) {
    if (!battery_img) return;
    if (current_screen == SCREEN_SPLASH) lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    else                                  lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
}

// Screen-level click handler — cycles forward through the three
// top-level screens (Splash → Usage → Bluetooth → Splash). Splash
// auto-morphs to Activity content when sessions are active, so there's
// no separate Activity cycle entry. The Bluetooth reset zone has its
// own callback that consumes the click first.
static void global_click_cb(lv_event_t* e) {
    (void)e;
    screen_t next;
    switch (ui_get_current_screen()) {
    case SCREEN_SPLASH:    next = SCREEN_USAGE;     break;
    case SCREEN_USAGE:     next = SCREEN_BLUETOOTH; break;
    case SCREEN_BLUETOOTH: next = SCREEN_SPLASH;    break;
    default:               next = SCREEN_SPLASH;    break;
    }
    ui_show_screen(next);
}

static void ble_reset_click_cb(lv_event_t* e) {
    (void)e;
    ble_clear_bonds();
}

// Fade duration for the splash <-> activity morph. Short enough to feel
// snappy, long enough to read as a transform rather than a hard cut.
#define SPLASH_MORPH_MS  280

// Decide what to show inside the splash "default" screen based on the
// daemon's session state. Cross-fades the splash animation against the
// activity widget tree so a fresh session arrives as a transform, not a
// hard swap. Safe to call when we're on a non-splash screen — it no-ops
// in that case because the splash + activity containers are already
// hidden by ui_show_screen().
static void apply_default_screen_state(void) {
    // Only fade on the actual splash↔activity transition. Re-renders
    // triggered by a swipe between sessions, or by fresh BLE data
    // while sessions are still present, should swap content instantly
    // — fading those would feel like a flicker.
    static bool last_morph_active = false;

    if (current_screen != SCREEN_SPLASH) return;
    const bool has_sessions = cached_activity.valid && cached_activity.session_count > 0;
    const bool state_changed = has_sessions != last_morph_active;
    last_morph_active = has_sessions;
    lv_obj_t* splash_root = splash_get_root();
    if (has_sessions) {
        lv_obj_clear_flag(activity_container, LV_OBJ_FLAG_HIDDEN);
        if (state_changed) {
            lv_obj_fade_in(activity_container, SPLASH_MORPH_MS, 0);
            if (splash_root && !lv_obj_has_flag(splash_root, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_fade_out(splash_root, SPLASH_MORPH_MS, 0);
            } else {
                splash_hide();
            }
        } else {
            // Just re-render, no fade.
            lv_obj_set_style_opa(activity_container, LV_OPA_COVER, 0);
            splash_hide();
        }
    } else {
        splash_show();
        if (splash_root) {
            lv_obj_set_style_opa(splash_root, LV_OPA_COVER, 0);
            if (state_changed) lv_obj_fade_in(splash_root, SPLASH_MORPH_MS, 0);
        }
        if (!lv_obj_has_flag(activity_container, LV_OBJ_FLAG_HIDDEN)) {
            if (state_changed) {
                lv_obj_fade_out(activity_container, SPLASH_MORPH_MS, 0);
            } else {
                lv_obj_add_flag(activity_container, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void ui_show_screen(screen_t screen) {
    lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(activity_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ble_container, LV_OBJ_FLAG_HIDDEN);
    splash_hide();
    // Reset opacity on the morphable containers so a leftover fade-out
    // from a previous transition doesn't render them invisible.
    lv_obj_set_style_opa(activity_container, LV_OPA_COVER, 0);
    if (splash_get_root()) lv_obj_set_style_opa(splash_get_root(), LV_OPA_COVER, 0);

    switch (screen) {
    case SCREEN_SPLASH:
        // Decided by apply_default_screen_state below — could be the
        // Clawd animation or the Activity widget tree depending on
        // whether any sessions are live.
        break;
    case SCREEN_USAGE:      lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_BLUETOOTH:  lv_obj_clear_flag(ble_container, LV_OBJ_FLAG_HIDDEN); break;
    default: break;
    }

    // Hide the logo overlay on the splash screen — both morph states
    // (Clawd animation or Activity title) want a clean left margin.
    if (logo_img) {
        if (screen == SCREEN_SPLASH) lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        else                          lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
    }

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
    apply_battery_visibility();
    apply_default_screen_state();
}

void ui_cycle_screen(void) {
    // Three-way cycle now: Usage → Bluetooth → Splash → Usage. Splash
    // morphs to Activity content automatically when sessions are live.
    screen_t next;
    switch (current_screen) {
    case SCREEN_SPLASH:    next = SCREEN_USAGE;     break;
    case SCREEN_USAGE:     next = SCREEN_BLUETOOTH; break;
    case SCREEN_BLUETOOTH: next = SCREEN_SPLASH;    break;
    default:               next = SCREEN_USAGE;     break;
    }
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
        idx = 4;
    } else if (percent < 0) {
        idx = 0;
    } else if (percent <= 10) {
        idx = 0;
    } else if (percent <= 35) {
        idx = 1;
    } else if (percent <= 75) {
        idx = 2;
    } else {
        idx = 3;
    }
    lv_image_set_src(battery_img, &battery_dscs[idx]);
    apply_battery_visibility();
}
