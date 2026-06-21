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
LV_FONT_DECLARE(font_styrene_12);
LV_FONT_DECLARE(font_mono_32);
LV_FONT_DECLARE(font_mono_18);
LV_FONT_DECLARE(font_mono_15);
LV_FONT_DECLARE(font_mono_11);
LV_FONT_DECLARE(font_tiempos_18);

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
    int16_t title_x_off;
    int16_t logo_scale;   // 256 = 100% (LV_SCALE_NONE)

    // Usage screen
    int16_t usage_panel_h;
    int16_t usage_panel_gap;
    int16_t usage_bar_y;
    int16_t usage_bar_h;
    int16_t usage_reset_y;
    const lv_font_t* usage_title_font;
    const lv_font_t* usage_pct_font;
    const lv_font_t* usage_pill_font;
    const lv_font_t* usage_reset_font;
    const lv_font_t* usage_status_font;

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
    L.title_x_off = 16;
    L.logo_scale = 256;

    if (c.height >= 460) {
        // Large layout — tuned for 480x480 (AMOLED-2.16).
        L.content_y = 100;
        L.usage_panel_h = 150;
        L.usage_panel_gap = 16;
        L.usage_bar_y = 56;
        L.usage_bar_h = 24;
        L.usage_reset_y = 94;
        L.usage_title_font  = &font_tiempos_56;
        L.usage_pct_font    = &font_styrene_48;
        L.usage_pill_font   = &font_styrene_28;
        L.usage_reset_font  = &font_styrene_28;
        L.usage_status_font = &font_mono_32;
        L.bt_info_panel_h = 160;
        L.bt_reset_zone_h = 110;
        L.bt_title_font    = &font_tiempos_56;
        L.bt_status_font   = &font_styrene_48;
        L.bt_device_font   = &font_styrene_28;
        L.bt_credit_1_font = &font_styrene_24;
        L.bt_credit_2_font = &font_styrene_20;
    } else if (c.width >= 300) {
        // Compact layout — tuned for 368x448 (AMOLED-1.8).
        L.content_y = 85;
        L.usage_panel_h = 130;
        L.usage_panel_gap = 12;
        L.usage_bar_y = 48;
        L.usage_bar_h = 24;
        L.usage_reset_y = 78;
        L.usage_title_font  = &font_tiempos_56;
        L.usage_pct_font    = &font_styrene_48;
        L.usage_pill_font   = &font_styrene_28;
        L.usage_reset_font  = &font_styrene_28;
        L.usage_status_font = &font_mono_32;
        L.bt_info_panel_h = 140;
        L.bt_reset_zone_h = 90;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_28;
        L.bt_device_font   = &font_styrene_20;
        L.bt_credit_1_font = &font_styrene_16;
        L.bt_credit_2_font = &font_styrene_14;
    } else {
        // Tiny layout — tuned for 240x284 (LCD-1.83). Narrow + short: title,
        // percentages, pills and status line all drop a size or two, and the
        // two usage panels shrink to fit above the bottom status line.
        L.margin = 14;
        L.title_y = 14;
        L.title_x_off = 0;
        L.logo_scale = 140;   // shrink the 80x80 logo to ~44px on the narrow screen
        L.content_y = 54;
        L.usage_panel_h = 88;
        L.usage_panel_gap = 8;
        L.usage_bar_y = 30;
        L.usage_bar_h = 18;
        L.usage_reset_y = 48;
        L.usage_title_font  = &font_tiempos_34;
        L.usage_pct_font    = &font_styrene_28;
        L.usage_pill_font   = &font_styrene_14;
        L.usage_reset_font  = &font_styrene_16;
        L.usage_status_font = &font_mono_18;
        L.bt_info_panel_h = 110;
        L.bt_reset_zone_h = 70;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_28;
        L.bt_device_font   = &font_styrene_16;
        L.bt_credit_1_font = &font_styrene_14;
        L.bt_credit_2_font = &font_styrene_12;
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
#define COL_FAINT     THEME_FAINT
#define COL_TRACK7    THEME_TRACK7

// ---- Usage screen widgets (single non-splash view) ----
// Redesigned "Empfehlung" layout: header (logo + wordmark + clock), SESSION
// hero (big %, bar with pace marker, pace verdict), 7-day sparkline, WEEKLY
// secondary, animated status line. Built inline in init_usage_screen().
static lv_obj_t* usage_container;
static lv_obj_t* usage_group;        // live usage content — toggled vs pair/idle overlays
static lv_obj_t* pair_group;         // pairing hint — shown when disconnected
static lv_obj_t* lbl_wordmark;       // "Usage"
static lv_obj_t* lbl_clock;          // top-right HH:MM (placeholder until daemon sends time)
static lv_obj_t* lbl_session_pct;    // big session number (no % sign)
static lv_obj_t* lbl_pct_sign;       // "%" — repositioned right after the number
static lv_obj_t* lbl_session_reset;  // "2h 47m left"
static lv_obj_t* bar_session;
static lv_obj_t* pace_marker;        // white tick on the session bar = expected pace
static lv_obj_t* pace_dot;           // colored dot in the pace verdict row
static lv_obj_t* lbl_pace;           // "Ueber/Unter Tempo - ..."
static lv_obj_t* bar_day[7];         // 7-day sparkline (today = rightmost)
static lv_obj_t* bar_weekly;
static lv_obj_t* lbl_weekly_pct;
static lv_obj_t* lbl_anim;           // status line: connection state + whimsical idle

// ---- Battery indicator (shared, on top) ----
static lv_obj_t* battery_img;
static bool      battery_present = false;  // set from ui_update_battery; hides icon when no cell
static lv_obj_t* logo_img;
static lv_image_dsc_t battery_dscs[5];  // empty, low, medium, full, charging

// ---- Live-data freshness → which usage sub-view to show ----
// usage panels when data is flowing, an idle "Zzz" screen when the host is
// connected but no usage update landed within DATA_FRESH_MS, the pairing hint
// when BLE is down. Re-evaluated every loop in ui_tick_anim().
static lv_obj_t* idle_group;            // the "Zzz" idle screen
static uint32_t  last_data_ms = 0;      // lv_tick when the last valid usage update landed
static bool      data_received = false; // any valid update since boot
static int       view_state = -1;       // -1 unknown / 0 pair / 1 idle / 2 usage
static const uint32_t DATA_FRESH_MS = 90000;  // usage counts as "live" within this window (daemon sends ~60s)

// ---- Shared ----
static lv_image_dsc_t logo_dsc;
static screen_t current_screen = SCREEN_USAGE;
static bool     s_ble_connected = false;   // cached BLE connection state
static uint32_t connected_at_ms = 0;       // when we last entered CONNECTED ("Connected" dwell)

// Animation state
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;
static uint32_t anim_msg_start = 0;

// Clock: the daemon sends a snapshot once per poll (~60s). We take it as a base
// and advance locally via lv_tick so the displayed time stays minute-accurate
// between updates instead of lagging up to a poll interval behind.
static int      clock_base_sec = -1;   // seconds-of-day at last sync, or -1
static uint32_t clock_base_ms  = 0;     // lv_tick when that sync arrived
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

static void init_icon_dsc_rgb565a8(lv_image_dsc_t* dsc, int w, int h, const uint8_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data = data;
    dsc->data_size = w * h * 3;
}

// Small upper-case section caption (SESSION / LETZTE 7 TAGE / WEEKLY).
static lv_obj_t* make_caption(lv_obj_t* parent, const char* text,
                              const lv_font_t* font, lv_color_t color) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_style_text_letter_space(lbl, 1, 0);
    return lbl;
}

// Compact reset time without the "Resets in " prefix: "47m" / "2h 47m" / "4d 3h".
static void format_reset_short(int mins, char* buf, size_t len) {
    if (mins < 0)         snprintf(buf, len, "--");
    else if (mins < 60)   snprintf(buf, len, "%dm", mins);
    else if (mins < 1440) snprintf(buf, len, "%dh %dm", mins / 60, mins % 60);
    else                  snprintf(buf, len, "%dd %dh", mins / 1440, (mins % 1440) / 60);
}

static void init_battery_icons(void) {
    init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_data);
    init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_W, ICON_BATTERY_LOW_H, icon_battery_low_data);
    init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_W, ICON_BATTERY_MEDIUM_H, icon_battery_medium_data);
    init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_W, ICON_BATTERY_FULL_H, icon_battery_full_data);
    init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_W, ICON_BATTERY_CHARGING_H, icon_battery_charging_data);
}

// ======== Usage Screen ========

// (The usage view is built inline in init_usage_screen() below.)

// Pairing hint — shown when disconnected so the screen isn't empty and the
// user knows how to (re)pair. Wording matches the 3-second release gesture.
static void build_pair_group(lv_obj_t* parent) {
    pair_group = lv_obj_create(parent);
    lv_obj_set_size(pair_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(pair_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(pair_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pair_group, 0, 0);
    lv_obj_set_style_pad_all(pair_group, 0, 0);
    lv_obj_clear_flag(pair_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* l1 = lv_label_create(pair_group);
    lv_label_set_text(l1, "To pair");
    lv_obj_set_style_text_font(l1, L.bt_status_font, 0);
    lv_obj_set_style_text_color(l1, COL_TEXT, 0);
    lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t* l2 = lv_label_create(pair_group);
    lv_label_set_text(l2, "hold the power button");
    lv_obj_set_style_text_font(l2, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l2, COL_DIM, 0);
    lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, 120);

    lv_obj_t* l3 = lv_label_create(pair_group);
    lv_label_set_text(l3, "for 3 seconds, then release");
    lv_obj_set_style_text_font(l3, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l3, COL_DIM, 0);
    lv_obj_align(l3, LV_ALIGN_TOP_MID, 0, 160);

    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);  // ui_update_ble_status decides
}

// Idle "Zzz" screen — shown when the host is connected but no usage update has
// landed recently (token expired, daemon down, host asleep…). Full-screen, like
// the pairing hint, so we never render hours-old numbers as if they were live.
static void build_idle_group(lv_obj_t* parent) {
    idle_group = lv_obj_create(parent);
    lv_obj_set_size(idle_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(idle_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(idle_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(idle_group, 0, 0);
    lv_obj_set_style_pad_all(idle_group, 0, 0);
    lv_obj_clear_flag(idle_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    // A shrunk-down sleeping creature (reused claudepix "expression sleep" art)
    // sits between the header and the status line; the animated "Listening…"
    // status line carries the words, so no extra text is needed here.
    lv_obj_t* creature = splash_mini_create(idle_group, "expression sleep", 160);
    if (creature) lv_obj_align(creature, LV_ALIGN_CENTER, 0, -20);

    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);  // update_view_state decides
}

// A borderless, transparent-or-filled child object with no padding/scroll —
// used for the pace marker, pace dot and sparkline bars.
static lv_obj_t* make_block(lv_obj_t* parent, int w, int h, lv_color_t color) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void init_usage_screen(lv_obj_t* scr) {
    const int PADX = 16;
    const int CW   = L.scr_w - 2 * PADX;   // content width (208 on 240px panel)

    usage_container = lv_obj_create(scr);
    lv_obj_set_size(usage_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_container, 0, 0);
    lv_obj_set_style_bg_opa(usage_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_container, 0, 0);
    lv_obj_set_style_pad_all(usage_container, 0, 0);
    lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(usage_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    // Live usage content — toggled against the pair/idle overlays as one unit.
    usage_group = lv_obj_create(usage_container);
    lv_obj_set_size(usage_group, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_group, 0, 0);
    lv_obj_set_style_bg_opa(usage_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_group, 0, 0);
    lv_obj_set_style_pad_all(usage_group, 0, 0);
    lv_obj_clear_flag(usage_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    // --- Header: logo + wordmark · clock ---
    logo_img = lv_image_create(usage_group);
    lv_image_set_src(logo_img, &logo_dsc);
    lv_image_set_pivot(logo_img, 0, 0);
    lv_image_set_scale(logo_img, 64);   // 80px art -> ~20px
    lv_obj_set_pos(logo_img, PADX, 13);

    lbl_wordmark = lv_label_create(usage_group);
    lv_label_set_text(lbl_wordmark, "Usage");
    lv_obj_set_style_text_font(lbl_wordmark, &font_tiempos_18, 0);
    lv_obj_set_style_text_color(lbl_wordmark, COL_TEXT, 0);
    lv_obj_set_pos(lbl_wordmark, PADX + 26, 15);

    lbl_clock = lv_label_create(usage_group);
    lv_label_set_text(lbl_clock, "--:--");
    lv_obj_set_style_text_font(lbl_clock, &font_mono_11, 0);
    lv_obj_set_style_text_color(lbl_clock, COL_FAINT, 0);
    lv_obj_set_style_text_letter_space(lbl_clock, 1, 0);
    lv_obj_align(lbl_clock, LV_ALIGN_TOP_RIGHT, -PADX, 18);

    // --- SESSION caption + reset ---
    lv_obj_t* s_cap = make_caption(usage_group, "SESSION", &font_styrene_12, COL_DIM);
    lv_obj_set_pos(s_cap, PADX, 44);

    lbl_session_reset = lv_label_create(usage_group);
    lv_label_set_text(lbl_session_reset, "--");
    lv_obj_set_style_text_font(lbl_session_reset, &font_mono_11, 0);
    lv_obj_set_style_text_color(lbl_session_reset, COL_DIM, 0);
    lv_obj_align(lbl_session_reset, LV_ALIGN_TOP_RIGHT, -PADX, 44);

    // --- Big session % ---
    lbl_session_pct = lv_label_create(usage_group);
    lv_label_set_text(lbl_session_pct, "--");
    lv_obj_set_style_text_font(lbl_session_pct, &font_styrene_48, 0);
    lv_obj_set_style_text_color(lbl_session_pct, COL_TEXT, 0);
    lv_obj_set_pos(lbl_session_pct, PADX, 60);

    lbl_pct_sign = lv_label_create(usage_group);
    lv_label_set_text(lbl_pct_sign, "%");
    lv_obj_set_style_text_font(lbl_pct_sign, &font_styrene_24, 0);
    lv_obj_set_style_text_color(lbl_pct_sign, COL_DIM, 0);
    lv_obj_set_pos(lbl_pct_sign, PADX + 56, 86);   // repositioned in ui_update

    // --- Session bar + pace marker ---
    bar_session = make_bar(usage_group, PADX, 124, CW, 14);
    lv_obj_set_style_radius(bar_session, 7, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_session, 7, LV_PART_INDICATOR);

    pace_marker = make_block(usage_group, 2, 18, COL_TEXT);
    lv_obj_set_pos(pace_marker, PADX, 122);   // x set in ui_update

    // --- Pace verdict: dot + text ---
    pace_dot = make_block(usage_group, 7, 7, COL_GREEN);
    lv_obj_set_style_radius(pace_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_pos(pace_dot, PADX, 150);

    lbl_pace = lv_label_create(usage_group);
    lv_label_set_text(lbl_pace, "");
    lv_obj_set_style_text_font(lbl_pace, &font_mono_11, 0);
    lv_obj_set_style_text_color(lbl_pace, COL_DIM, 0);
    lv_obj_set_pos(lbl_pace, PADX + 13, 147);

    // --- LETZTE 7 TAGE sparkline ---
    lv_obj_t* d_cap = make_caption(usage_group, "LETZTE 7 TAGE", &font_styrene_12, COL_FAINT);
    lv_obj_set_pos(d_cap, PADX, 172);

    lv_obj_t* days = lv_obj_create(usage_group);
    lv_obj_set_pos(days, PADX, 189);
    lv_obj_set_size(days, CW, 30);
    lv_obj_set_style_bg_opa(days, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(days, 0, 0);
    lv_obj_set_style_pad_all(days, 0, 0);
    lv_obj_clear_flag(days, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(days, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(days, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    static const int dh[7] = {18, 12, 23, 30, 16, 21, 13};   // placeholder heights
    for (int i = 0; i < 7; i++) {
        bar_day[i] = make_block(days, 25, dh[i], COL_TRACK7);
        lv_obj_set_style_radius(bar_day[i], 2, 0);
    }

    // --- WEEKLY secondary: flex row [label · bar · %], equal gaps via pad_column ---
    lv_obj_t* wk = lv_obj_create(usage_group);
    lv_obj_set_pos(wk, PADX, 227);
    lv_obj_set_size(wk, CW, 16);
    lv_obj_set_style_bg_opa(wk, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wk, 0, 0);
    lv_obj_set_style_pad_all(wk, 0, 0);
    lv_obj_set_style_pad_column(wk, 10, 0);   // same gap left & right of the bar
    lv_obj_clear_flag(wk, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(wk, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wk, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    make_caption(wk, "WEEKLY", &font_styrene_12, COL_DIM);

    bar_weekly = lv_bar_create(wk);
    lv_obj_set_height(bar_weekly, 6);
    lv_obj_set_flex_grow(bar_weekly, 1);   // fills the middle; gaps stay equal
    lv_bar_set_range(bar_weekly, 0, 100);
    lv_bar_set_value(bar_weekly, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_weekly, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_weekly, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_weekly, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_weekly, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar_weekly, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_weekly, 3, LV_PART_INDICATOR);

    lbl_weekly_pct = lv_label_create(wk);
    lv_label_set_text(lbl_weekly_pct, "--%");
    lv_obj_set_style_text_font(lbl_weekly_pct, &font_mono_11, 0);
    lv_obj_set_style_text_color(lbl_weekly_pct, COL_DIM, 0);

    build_pair_group(usage_container);
    build_idle_group(usage_container);

    // Status line — always visible on the usage view. Driven by ui_tick_anim().
    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, &font_mono_15, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -12);
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
    splash_init(scr);

    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    // Logo lives in the usage-screen header now; no battery icon on this layout.
}

void ui_update(const UsageData* data) {
    if (!data->valid) return;
    last_data_ms = lv_tick_get();   // a valid usage update just landed → dot goes green
    data_received = true;

    const int PADX = 16;
    int s_pct = (int)(data->session_pct + 0.5f);
    int w_pct = (int)(data->weekly_pct + 0.5f);
    lv_color_t s_col = pct_color(data->session_pct);

    // Big session number; the "%" sign trails it at the same baseline.
    lv_label_set_text_fmt(lbl_session_pct, "%d", s_pct);
    lv_obj_update_layout(lbl_session_pct);
    lv_obj_set_pos(lbl_pct_sign, PADX + lv_obj_get_width(lbl_session_pct) + 3, 86);

    lv_bar_set_value(bar_session, s_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_session, s_col, LV_PART_INDICATOR);

    char buf[32];
    format_reset_short(data->session_reset_mins, buf, sizeof(buf));
    lv_label_set_text_fmt(lbl_session_reset, "%s left", buf);

    // Pace marker = elapsed share of the 5h (300 min) session window. The fill
    // left of the marker is your buffer; past it means you're burning ahead.
    int reset = data->session_reset_mins;
    int elapsed = 300 - (reset < 0 ? 300 : reset);
    if (elapsed < 0) elapsed = 0;
    if (elapsed > 300) elapsed = 300;
    int pace = elapsed * 100 / 300;
    int track_w = lv_obj_get_width(bar_session);
    if (track_w <= 0) track_w = L.scr_w - 2 * PADX;
    lv_obj_set_pos(pace_marker, PADX + pace * track_w / 100, 122);

    bool ahead = s_pct > pace;
    lv_obj_set_style_bg_color(pace_dot,
        ahead ? (s_pct >= 80 ? COL_RED : COL_AMBER) : COL_GREEN, 0);
    // Mono font carries the umlaut + middot glyphs. Split the \xC3\x9C (Ü)
    // escape from "ber" — a \x escape greedily eats following hex digits (b,e).
    lv_label_set_text(lbl_pace, ahead ? "\xC3\x9C" "ber Tempo \xC2\xB7 Cap droht"
                                      : "Unter Tempo \xC2\xB7 Reset vor Limit");

    // Clock: capture host time (HH:MM:SS) to the second; ui_tick_anim advances
    // it locally so the minute flips at the real boundary, not the poll instant.
    if (data->clock[0] >= '0' && data->clock[0] <= '2' && data->clock[2] == ':') {
        int hh = (data->clock[0] - '0') * 10 + (data->clock[1] - '0');
        int mm = (data->clock[3] - '0') * 10 + (data->clock[4] - '0');
        int ss = (data->clock[5] == ':') ? (data->clock[6] - '0') * 10 + (data->clock[7] - '0') : 0;
        clock_base_sec = hh * 3600 + mm * 60 + ss;
        clock_base_ms = lv_tick_get();
    }

    // 7-day sparkline: heights from the daemon (0-100 → 2..30px), today coloured.
    for (int i = 0; i < 7; i++) {
        if (!bar_day[i]) continue;
        int hpx = 2 + (int)data->day7[i] * 28 / 100;
        lv_obj_set_height(bar_day[i], hpx);
        lv_obj_set_style_bg_color(bar_day[i], i == 6 ? s_col : COL_TRACK7, 0);
    }

    lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", w_pct);
    lv_bar_set_value(bar_weekly, w_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_weekly, pct_color(data->weekly_pct), LV_PART_INDICATOR);
}

// Pick the usage-view sub-screen: pairing hint (BLE down), the idle "Zzz" screen
// (connected but data has gone stale), or the live usage panels. Only re-lays-out
// on an actual change. The animated status line stays visible everywhere — it
// reads "Listening…" on the idle screen, keeping it alive rather than frozen.
static void update_view_state(void) {
    if (!usage_group || !pair_group || !idle_group) return;
    int v;
    if (!s_ble_connected) {
        v = 0;  // pairing hint
    } else if (data_received && (lv_tick_get() - last_data_ms) < DATA_FRESH_MS) {
        v = 2;  // live usage
    } else {
        v = 1;  // idle / Zzz
    }
    if (v == view_state) return;
    view_state = v;
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(v == 0 ? pair_group : v == 1 ? idle_group : usage_group,
                      LV_OBJ_FLAG_HIDDEN);
}

// Advance the clock locally from the last host sync so it doesn't lag the poll.
static void update_clock(void) {
    if (clock_base_sec < 0 || !lbl_clock) return;
    uint32_t elapsed = (lv_tick_get() - clock_base_ms) / 1000;
    int cur = (clock_base_sec + (int)elapsed) % 86400;
    lv_label_set_text_fmt(lbl_clock, "%02d:%02d", cur / 3600, (cur % 3600) / 60);
}

void ui_tick_anim(void) {
    if (current_screen != SCREEN_USAGE) return;
    update_view_state();
    update_clock();
    if (view_state == 1) splash_mini_tick();   // animate the sleeping creature on the idle screen

    uint32_t now = lv_tick_get();

    if (now - anim_msg_start >= ANIM_MSG_MS) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_msg_start = now;
    }

    if (now - anim_last_ms < spinner_ms[anim_spinner_idx]) return;
    anim_last_ms = now;
    anim_phase = (anim_phase + 1) % SPINNER_PHASES;
    anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                    : (SPINNER_PHASES - anim_phase);

    // Status text by priority. Whimsical messages only when connected & settled.
    const char* text;
    if (!s_ble_connected) {
        text = "Waiting";              // advertising / waiting for a host connection
    } else if (view_state == 1) {      // idle — alternate so it reads as alive AND data-less
        text = (anim_msg_idx & 1) ? "No data" : "Listening";
    } else if (now - connected_at_ms < 5000) {
        text = "Connected";
    } else {
        text = anim_messages[anim_msg_idx];
    }

    // All states share the whimsical style: "<glyph> <Title-case word>…"
    static char buf[80];
    snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
             spinner_frames[anim_spinner_idx], text);
    lv_label_set_text(lbl_anim, buf);
}

static screen_t prev_non_splash_screen = SCREEN_USAGE;
static void apply_battery_visibility(void) {
    if (!battery_img) return;
    // Hidden on the splash screen, and whenever no battery is present — some
    // boards ship without a cell (PMU populated, no battery).
    bool show = battery_present && current_screen != SCREEN_SPLASH;
    if (show) lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
}

static void global_click_cb(lv_event_t* e) {
    (void)e;
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

void ui_show_screen(screen_t screen) {
    lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:  splash_show(); break;
    case SCREEN_USAGE:   lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN); break;
    default: break;
    }

    if (logo_img) {
        if (screen == SCREEN_SPLASH) lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        else                          lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
    }

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
    apply_battery_visibility();
}

void ui_toggle_splash(void) {
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    (void)name; (void)mac;
    bool was_connected = s_ble_connected;
    s_ble_connected = (state == BLE_STATE_CONNECTED);

    if (s_ble_connected && !was_connected) connected_at_ms = lv_tick_get();
    // pair / idle / usage — picked from connection + data freshness.
    update_view_state();
}

void ui_update_battery(int percent, bool charging) {
    // Hide the icon unless the board actually has a battery AND a cell is
    // detected. Boards without a battery (has_battery=false) never show it;
    // battery-optional boards hide it when power_hal reports percent < 0.
    (void)charging;
    battery_present = board_caps().has_battery && (percent >= 0);
    if (battery_present) {
        int idx;
        if (charging)           idx = 4;
        else if (percent <= 10) idx = 0;
        else if (percent <= 35) idx = 1;
        else if (percent <= 75) idx = 2;
        else                    idx = 3;
        lv_image_set_src(battery_img, &battery_dscs[idx]);
    }
    apply_battery_visibility();
}
