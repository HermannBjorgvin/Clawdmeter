#include "ui.h"
#include "activity_freshness.h"
#include "activity_style.h"
#include "dashboard_branding.h"
#include "dashboard_payload.h"
#include "splash.h"
#include <lvgl.h>
#include "logo.h"
#include "codex_logo.h"
#include "icons.h"
#include "hal/board_caps.h"
#include "ui_layout.h"
#include "usage_view_state.h"

// Custom fonts (scaled for 314 PPI, ~1.9x from original 165 PPI)
LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_mono_18);
LV_FONT_DECLARE(font_mono_32);

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
    int16_t panel_width;
    int16_t second_panel_x;

    // Usage screen
    int16_t usage_panel_h;
    int16_t usage_panel_gap;
    int16_t usage_bar_y;
    int16_t usage_reset_y;
    int16_t usage_description_y;
    int16_t usage_status_y;
    int16_t logo_size;
    int16_t logo_scale;
    int16_t logo_rendered_width;
    bool horizontal_cards;
    int16_t footer_y;
    int16_t page_indicator_y;
    bool claude_compact_rows;
    int16_t claude_row_y;
    int16_t claude_row_h;
    int16_t claude_row_gap;
    int16_t claude_bar_h;
    int16_t claude_status_x;
    int16_t claude_status_y;
    int16_t claude_status_w;

    // Bluetooth screen
    int16_t bt_info_panel_h;
    int16_t bt_reset_zone_h;
    int16_t pairing_title_y;
    int16_t pairing_instruction_y;
    int16_t pairing_release_y;
    const lv_font_t* bt_title_font;
    const lv_font_t* bt_status_font;
    const lv_font_t* bt_device_font;
    const lv_font_t* bt_credit_1_font;
    const lv_font_t* bt_credit_2_font;
    const lv_font_t* title_font;
    const lv_font_t* percentage_font;
    const lv_font_t* reset_font;
    const lv_font_t* pill_font;
    const lv_font_t* detail_font;
    const lv_font_t* status_font;
    int16_t idle_creature_size;
};
static Layout L = {};

// Pick layout values from the active board's pixel dimensions. The two
// existing boards happen to land on the two breakpoints below; new ports
// inherit the closer one — visually OK, may need a polish pass for
// pixel-perfect alignment but never blocks the port from booting.
static void compute_layout(const BoardCaps& c) {
    const UiLayoutMetrics metrics = compute_ui_layout_metrics(c.width, c.height);
    L.scr_w = metrics.screen_width;
    L.scr_h = metrics.screen_height;
    L.margin = metrics.margin;
    L.title_y = metrics.title_y;
    L.content_y = metrics.content_y;
    L.content_w = metrics.content_width;
    L.panel_width = metrics.panel_width;
    L.second_panel_x = metrics.second_panel_x;
    L.usage_panel_h = metrics.usage_panel_h;
    L.usage_panel_gap = metrics.usage_panel_gap;
    L.usage_bar_y = metrics.usage_bar_y;
    L.usage_reset_y = metrics.usage_reset_y;
    L.usage_description_y = metrics.usage_description_y;
    L.usage_status_y = metrics.usage_status_y;
    L.logo_size = metrics.logo_size;
    L.logo_scale = metrics.logo_scale;
    L.logo_rendered_width = metrics.logo_rendered_width;
    L.horizontal_cards = metrics.horizontal_cards;
    L.footer_y = metrics.footer_y;
    L.page_indicator_y = metrics.page_indicator_y;
    L.claude_compact_rows = metrics.claude_compact_rows;
    L.claude_row_y = metrics.claude_row_y;
    L.claude_row_h = metrics.claude_row_h;
    L.claude_row_gap = metrics.claude_row_gap;
    L.claude_bar_h = metrics.claude_bar_h;
    L.claude_status_x = metrics.claude_status_x;
    L.claude_status_y = metrics.claude_status_y;
    L.claude_status_w = metrics.claude_status_w;
    L.bt_info_panel_h = metrics.bluetooth_panel_h;
    L.bt_reset_zone_h = metrics.bluetooth_reset_zone_h;
    L.pairing_title_y = metrics.pairing_title_y;
    L.pairing_instruction_y = metrics.pairing_instruction_y;
    L.pairing_release_y = metrics.pairing_release_y;
    L.idle_creature_size = metrics.idle_creature_size;

    L.title_font = metrics.small_display ? &font_tiempos_34 : &font_tiempos_56;
    L.percentage_font = metrics.percentage_font_px == 24
        ? &font_styrene_24
        : &font_styrene_48;
    L.reset_font = metrics.small_display ? &font_styrene_14 : &font_styrene_28;
    L.pill_font = metrics.small_display ? &font_styrene_14 : &font_styrene_28;
    L.detail_font = metrics.small_display ? &font_styrene_14 : &font_styrene_16;
    L.status_font = metrics.small_display ? &font_mono_18 : &font_mono_32;

    if (metrics.small_display) {
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_28;
        L.bt_device_font   = &font_styrene_14;
        L.bt_credit_1_font = &font_styrene_14;
        L.bt_credit_2_font = &font_styrene_14;
    } else if (c.height >= 460) {
        // Large layout — tuned for 480x480 (AMOLED-2.16).
        L.bt_title_font    = &font_tiempos_56;
        L.bt_status_font   = &font_styrene_48;
        L.bt_device_font   = &font_styrene_28;
        L.bt_credit_1_font = &font_styrene_24;
        L.bt_credit_2_font = &font_styrene_20;
    } else {
        // Compact layout — tuned for 368x448 (AMOLED-1.8).
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_28;
        L.bt_device_font   = &font_styrene_20;
        L.bt_credit_1_font = &font_styrene_16;
        L.bt_credit_2_font = &font_styrene_14;
    }
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
#define COL_MUTED     THEME_DIM

const char* codex_window_label(int window_mins) {
    if (window_mins == 300) return "5 hours";
    if (window_mins == 10080) return "Weekly";
    return "Limit";
}

void format_compact_tokens(uint32_t tokens, char* buffer, size_t length) {
    if (tokens >= 1000000U) {
        snprintf(buffer, length, "%.1fM", tokens / 1000000.0f);
    } else if (tokens >= 1000U) {
        snprintf(buffer, length, "%.1fk", tokens / 1000.0f);
    } else {
        snprintf(buffer, length, "%lu", static_cast<unsigned long>(tokens));
    }
}

// ---- Claude usage screen widgets ----
static lv_obj_t* claude_container;
static lv_obj_t* lbl_title;
static lv_obj_t* usage_group;   // the two usage panels — shown when connected
static lv_obj_t* pair_group;    // pairing hint — shown when disconnected
static lv_obj_t* bar_session;
static lv_obj_t* lbl_session_pct;
static lv_obj_t* lbl_session_label;
static lv_obj_t* lbl_session_reset;
static lv_obj_t* bar_weekly;
static lv_obj_t* lbl_weekly_pct;
static lv_obj_t* lbl_weekly_label;
static lv_obj_t* lbl_weekly_reset;
static lv_obj_t* panel_session = nullptr;
static lv_obj_t* panel_weekly = nullptr;
static lv_obj_t* panel_fable = nullptr;
static lv_obj_t* lbl_fable_pct = nullptr;
static lv_obj_t* lbl_fable_label = nullptr;
static lv_obj_t* bar_fable = nullptr;
static lv_obj_t* lbl_fable_reset = nullptr;
// Enterprise-only widgets inside panel_session
static lv_obj_t* lbl_session_pct_sym = nullptr;  // "%" in smaller font
static lv_obj_t* lbl_spending_desc = nullptr;     // "of your monthly budget"
static lv_obj_t* lbl_spending_status = nullptr;   // "Under pace" / "On pace" / "Over pace"
static lv_obj_t* lbl_anim;      // status line: connection state + whimsical idle

// ---- Codex usage screen widgets ----
static lv_obj_t* codex_container;
static lv_obj_t* codex_pct[2];
static lv_obj_t* codex_label[2];
static lv_obj_t* codex_bar[2];
static lv_obj_t* codex_reset[2];

// ---- Activity screen widgets ----
static lv_obj_t* activity_container;
static lv_obj_t* lbl_activity_claude_title;
static lv_obj_t* lbl_activity_claude_unavailable;
static lv_obj_t* lbl_activity_open;
static lv_obj_t* lbl_activity_busy;
static lv_obj_t* lbl_activity_waiting;
static lv_obj_t* lbl_activity_codex_title;
static lv_obj_t* lbl_activity_codex_unavailable;
static lv_obj_t* lbl_activity_unread;
static lv_obj_t* activity_footer_label;
static ActivityFreshnessState activity_freshness = {};
static uint32_t last_activity_footer_refresh_ms = 0;

// ---- Metric page indicators ----
static lv_obj_t* page_indicator_group;
static lv_obj_t* page_indicator_dots[DASHBOARD_PAGE_COUNT];

// ---- Battery indicator (shared, on top) ----
static lv_obj_t* battery_img;
static lv_obj_t* claude_logo_img;
static lv_obj_t* codex_logo_img;
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
static lv_image_dsc_t claude_logo_dsc;
static lv_image_dsc_t codex_logo_dsc;
static CarouselState carousel = {};
static DashboardVisibilityState dashboard_visibility = {};
static DashboardPage current_page = DASHBOARD_CLAUDE;
static lv_obj_t* navigation_layer = nullptr;
static bool     s_ble_connected = false;   // cached BLE connection state
static uint32_t connected_at_ms = 0;       // when we last entered CONNECTED ("Connected" dwell)

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

static void format_compact_reset_time(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "%dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "%dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "%dd %dh", mins / 1440, (mins % 1440) / 60);
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

static lv_obj_t* make_pill(lv_obj_t* parent, const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, L.pill_font, 0);
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
    if (L.scr_w <= 240 && L.scr_h <= 240) {
        init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_SMALL_W, ICON_BATTERY_SMALL_H, icon_battery_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_SMALL_W, ICON_BATTERY_LOW_SMALL_H, icon_battery_low_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_SMALL_W, ICON_BATTERY_MEDIUM_SMALL_H, icon_battery_medium_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_SMALL_W, ICON_BATTERY_FULL_SMALL_H, icon_battery_full_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_SMALL_W, ICON_BATTERY_CHARGING_SMALL_H, icon_battery_charging_small_data);
        return;
    }

    init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_data);
    init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_W, ICON_BATTERY_LOW_H, icon_battery_low_data);
    init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_W, ICON_BATTERY_MEDIUM_H, icon_battery_medium_data);
    init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_W, ICON_BATTERY_FULL_H, icon_battery_full_data);
    init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_W, ICON_BATTERY_CHARGING_H, icon_battery_charging_data);
}

// ======== Usage Screen ========

static lv_obj_t* make_usage_panel(
    lv_obj_t* parent,
    int x,
    int y,
    int width,
    const char* pill_text,
    lv_obj_t** out_pct,
    lv_obj_t** out_pill,
    lv_obj_t** out_bar,
    lv_obj_t** out_reset
) {
    lv_obj_t* panel = make_panel(parent, x, y, width, L.usage_panel_h);
    const int panel_padding = L.horizontal_cards ? 8 : 16;
    const int content_width = width - (2 * panel_padding);
    if (L.horizontal_cards) lv_obj_set_style_pad_all(panel, panel_padding, 0);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, L.percentage_font, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, L.horizontal_cards ? 28 : 0);

    *out_pill = make_pill(panel, pill_text);
    if (L.horizontal_cards) {
        lv_obj_set_width(*out_pill, content_width);
        lv_label_set_long_mode(*out_pill, LV_LABEL_LONG_DOT);
    }
    lv_obj_align(
        *out_pill,
        L.horizontal_cards ? LV_ALIGN_TOP_LEFT : LV_ALIGN_TOP_RIGHT,
        0,
        L.horizontal_cards ? 0 : 1
    );

    *out_bar = make_bar(panel, 0, L.usage_bar_y, content_width, 24);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, L.reset_font, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, L.usage_reset_y);
    if (L.horizontal_cards) {
        lv_obj_set_width(*out_reset, content_width);
        lv_label_set_long_mode(*out_reset, LV_LABEL_LONG_DOT);
    }

    return panel;
}

static lv_obj_t* make_compact_usage_row(
    lv_obj_t* parent,
    int y,
    const char* label,
    lv_obj_t** out_pct,
    lv_obj_t** out_label,
    lv_obj_t** out_bar,
    lv_obj_t** out_reset
) {
    lv_obj_t* panel = make_panel(
        parent, L.margin, y, L.content_w, L.claude_row_h
    );
    lv_obj_set_style_pad_all(panel, 5, 0);
    const int content_width = L.content_w - 10;

    *out_label = lv_label_create(panel);
    lv_label_set_text(*out_label, label);
    lv_obj_set_style_text_font(*out_label, &font_styrene_14, 0);
    lv_obj_set_style_text_color(*out_label, COL_TEXT, 0);
    lv_obj_set_pos(*out_label, 0, 0);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_width(*out_reset, 118);
    lv_label_set_long_mode(*out_reset, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(*out_reset, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(*out_reset, &font_styrene_14, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, content_width - 166, 0);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "--");
    lv_obj_set_style_text_font(*out_pct, &font_styrene_16, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_align(*out_pct, LV_ALIGN_TOP_RIGHT, 0, -1);

    *out_bar = make_bar(panel, 0, 27, content_width, L.claude_bar_h);
    return panel;
}

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
    lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, L.pairing_title_y);

    lv_obj_t* l2 = lv_label_create(pair_group);
    lv_label_set_text(l2, "hold the power button");
    lv_obj_set_style_text_font(l2, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l2, COL_DIM, 0);
    lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, L.pairing_instruction_y);

    lv_obj_t* l3 = lv_label_create(pair_group);
    lv_label_set_text(l3, "for 3 seconds, then release");
    lv_obj_set_style_text_font(l3, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l3, COL_DIM, 0);
    lv_obj_align(l3, LV_ALIGN_TOP_MID, 0, L.pairing_release_y);

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
    lv_obj_t* creature = splash_mini_create(
        idle_group,
        "expression sleep",
        L.idle_creature_size
    );
    if (creature) lv_obj_align(creature, LV_ALIGN_CENTER, 0, -20);

    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);  // update_view_state decides
}

static void init_usage_screen(lv_obj_t* scr) {
    claude_container = lv_obj_create(scr);
    lv_obj_set_size(claude_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(claude_container, 0, 0);
    lv_obj_set_style_bg_opa(claude_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(claude_container, 0, 0);
    lv_obj_set_style_pad_all(claude_container, 0, 0);
    lv_obj_clear_flag(claude_container, LV_OBJ_FLAG_SCROLLABLE);

    lbl_title = lv_label_create(claude_container);
    lv_label_set_text(lbl_title, "Claude");
    lv_obj_set_style_text_font(lbl_title, L.title_font, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, L.title_y);

    // Usage panels (shown when connected) live in a transparent full-size group
    // so they can be toggled against the pairing hint as one unit.
    usage_group = lv_obj_create(claude_container);
    lv_obj_set_size(usage_group, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_group, 0, 0);
    lv_obj_set_style_bg_opa(usage_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_group, 0, 0);
    lv_obj_set_style_pad_all(usage_group, 0, 0);
    lv_obj_clear_flag(usage_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    if (L.claude_compact_rows) {
        const int row_step = L.claude_row_h + L.claude_row_gap;
        panel_session = make_compact_usage_row(
            usage_group, L.claude_row_y, "Currently",
            &lbl_session_pct, &lbl_session_label,
            &bar_session, &lbl_session_reset
        );
        panel_weekly = make_compact_usage_row(
            usage_group, L.claude_row_y + row_step, "Weekly",
            &lbl_weekly_pct, &lbl_weekly_label,
            &bar_weekly, &lbl_weekly_reset
        );
        panel_fable = make_compact_usage_row(
            usage_group, L.claude_row_y + (2 * row_step), "Fable",
            &lbl_fable_pct, &lbl_fable_label,
            &bar_fable, &lbl_fable_reset
        );
    } else {
        panel_session = make_usage_panel(
            usage_group, L.margin, L.content_y, L.panel_width, "Current",
            &lbl_session_pct, &lbl_session_label,
            &bar_session, &lbl_session_reset
        );

        const int second_x = L.horizontal_cards ? L.second_panel_x : L.margin;
        const int second_y = L.horizontal_cards
            ? L.content_y
            : L.content_y + L.usage_panel_h + L.usage_panel_gap;
        panel_weekly = make_usage_panel(
            usage_group, second_x, second_y, L.panel_width, "Weekly",
            &lbl_weekly_pct, &lbl_weekly_label,
            &bar_weekly, &lbl_weekly_reset
        );
        lv_label_set_recolor(lbl_weekly_reset, true);
    }

    // Enterprise-only overlays inside panel_session — hidden until enterprise data arrives
    lbl_session_pct_sym = lv_label_create(panel_session);
    lv_label_set_text(lbl_session_pct_sym, "%");
    lv_obj_set_style_text_font(lbl_session_pct_sym, L.reset_font, 0);
    lv_obj_set_style_text_color(lbl_session_pct_sym, COL_TEXT, 0);
    lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_desc = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_desc, "of your monthly budget");
    lv_obj_set_style_text_font(lbl_spending_desc, L.reset_font, 0);
    lv_obj_set_style_text_color(lbl_spending_desc, COL_DIM, 0);
    const int usage_content_width = L.claude_compact_rows
        ? L.content_w - 10
        : L.panel_width - (2 * (L.horizontal_cards ? 8 : 16));
    lv_obj_set_width(lbl_spending_desc, usage_content_width);
    lv_label_set_long_mode(lbl_spending_desc, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(lbl_spending_desc, 0, L.usage_description_y);
    lv_obj_add_flag(lbl_spending_desc, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_status = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_status, "");
    lv_obj_set_style_text_font(lbl_spending_status, L.detail_font, 0);
    lv_obj_set_width(lbl_spending_status, usage_content_width);
    lv_label_set_long_mode(lbl_spending_status, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(lbl_spending_status, 0, L.usage_status_y);
    lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);

    build_pair_group(claude_container);
    build_idle_group(claude_container);

    // Status line — always visible on the usage view. Driven by ui_tick_anim().
    lbl_anim = lv_label_create(claude_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    if (L.claude_compact_rows) {
        lv_obj_set_width(lbl_anim, L.claude_status_w);
        lv_label_set_long_mode(lbl_anim, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(lbl_anim, &font_styrene_14, 0);
        lv_obj_set_pos(lbl_anim, L.claude_status_x, L.claude_status_y);
    } else {
        lv_obj_set_style_text_font(lbl_anim, L.status_font, 0);
        lv_obj_align(lbl_anim, LV_ALIGN_TOP_MID, 0, L.footer_y);
    }
}

// ======== Public API ========

void ui_init(void) {
    compute_layout(board_caps());

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    init_icon_dsc_rgb565a8(
        &claude_logo_dsc, LOGO_WIDTH, LOGO_HEIGHT, logo_data
    );
    init_icon_dsc_rgb565a8(
        &codex_logo_dsc, CODEX_LOGO_WIDTH, CODEX_LOGO_HEIGHT, codex_logo_data
    );

    init_usage_screen(scr);
    splash_init(scr);

    codex_container = lv_obj_create(scr);
    lv_obj_set_size(codex_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(codex_container, 0, 0);
    lv_obj_set_style_bg_opa(codex_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(codex_container, 0, 0);
    lv_obj_set_style_pad_all(codex_container, 0, 0);
    lv_obj_clear_flag(codex_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(codex_container, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* codex_title = lv_label_create(codex_container);
    lv_label_set_text(codex_title, "Codex");
    lv_obj_set_style_text_font(codex_title, L.title_font, 0);
    lv_obj_set_style_text_color(codex_title, COL_TEXT, 0);
    lv_obj_align(codex_title, LV_ALIGN_TOP_MID, 0, L.title_y);

    make_usage_panel(codex_container, L.margin, L.content_y, L.panel_width, "Limit",
                     &codex_pct[0], &codex_label[0],
                     &codex_bar[0], &codex_reset[0]);
    const int second_x = L.horizontal_cards ? L.second_panel_x : L.margin;
    const int second_y = L.horizontal_cards
        ? L.content_y
        : L.content_y + L.usage_panel_h + L.usage_panel_gap;
    make_usage_panel(codex_container, second_x, second_y, L.panel_width,
                     "Tokens today", &codex_pct[1], &codex_label[1],
                     &codex_bar[1], &codex_reset[1]);
    if (L.scr_h <= 320) {
        for (lv_obj_t* label : codex_label) {
            lv_obj_set_style_pad_left(label, 8, 0);
            lv_obj_set_style_pad_right(label, 8, 0);
        }
    }
    lv_label_set_text(codex_pct[0], "Codex");
    lv_label_set_text(codex_reset[0], "Unavailable");
    lv_obj_add_flag(codex_label[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(codex_bar[0], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(codex_pct[1], "---");
    lv_label_set_text(codex_reset[1], "Unavailable");
    lv_obj_add_flag(codex_bar[1], LV_OBJ_FLAG_HIDDEN);

    activity_container = lv_obj_create(scr);
    lv_obj_set_size(activity_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(activity_container, 0, 0);
    lv_obj_set_style_bg_opa(activity_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(activity_container, 0, 0);
    lv_obj_set_style_pad_all(activity_container, 0, 0);
    lv_obj_clear_flag(activity_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(activity_container, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* activity_title = lv_label_create(activity_container);
    lv_label_set_text(activity_title, "Activity");
    lv_obj_set_style_text_font(activity_title, L.title_font, 0);
    lv_obj_set_style_text_color(activity_title, COL_TEXT, 0);
    lv_obj_align(activity_title, LV_ALIGN_TOP_MID, 0, L.title_y);

    lv_obj_t* claude_activity_panel = make_panel(
        activity_container, L.margin, L.content_y, L.panel_width, L.usage_panel_h
    );
    lv_obj_t* codex_activity_panel = make_panel(
        activity_container, second_x, second_y, L.panel_width, L.usage_panel_h
    );
    if (L.scr_h <= 320) {
        lv_obj_set_style_pad_all(claude_activity_panel, 8, 0);
        lv_obj_set_style_pad_all(codex_activity_panel, 8, 0);
    }

    const bool compact_activity = L.usage_panel_h <= 90;
    const int activity_content_width =
        L.panel_width - (L.scr_h <= 320 ? 16 : 32);

    lbl_activity_claude_title = lv_label_create(claude_activity_panel);
    lv_label_set_text(lbl_activity_claude_title, "Claude Code");
    lv_obj_set_width(lbl_activity_claude_title, activity_content_width);
    lv_obj_set_style_text_font(lbl_activity_claude_title, L.detail_font, 0);
    lv_obj_set_style_text_color(lbl_activity_claude_title, COL_TEXT, 0);
    lv_obj_set_pos(lbl_activity_claude_title, 0, 0);

    lbl_activity_claude_unavailable = lv_label_create(claude_activity_panel);
    lv_label_set_text(lbl_activity_claude_unavailable, "Unavailable");
    lv_obj_set_width(lbl_activity_claude_unavailable, activity_content_width);
    lv_obj_set_style_text_font(lbl_activity_claude_unavailable, L.detail_font, 0);
    lv_obj_set_style_text_color(lbl_activity_claude_unavailable, COL_MUTED, 0);
    lv_obj_set_pos(
        lbl_activity_claude_unavailable, 0, compact_activity ? 20 : 28
    );

    lbl_activity_open = lv_label_create(claude_activity_panel);
    lv_label_set_text(lbl_activity_open, "");
    lv_obj_set_width(lbl_activity_open, activity_content_width);
    lv_obj_set_style_text_font(lbl_activity_open, L.detail_font, 0);
    lv_obj_set_style_text_color(
        lbl_activity_open, lv_color_hex(ACTIVITY_OPEN_HEX), 0
    );
    lv_obj_set_pos(lbl_activity_open, 0, compact_activity ? 20 : 28);
    lv_obj_add_flag(lbl_activity_open, LV_OBJ_FLAG_HIDDEN);

    lbl_activity_busy = lv_label_create(claude_activity_panel);
    lv_label_set_text(lbl_activity_busy, "");
    lv_obj_set_width(lbl_activity_busy, activity_content_width);
    lv_obj_set_style_text_font(lbl_activity_busy, L.detail_font, 0);
    lv_obj_set_style_text_color(
        lbl_activity_busy, lv_color_hex(ACTIVITY_BUSY_HEX), 0
    );
    lv_obj_set_pos(lbl_activity_busy, 0, compact_activity ? 40 : 52);
    lv_obj_add_flag(lbl_activity_busy, LV_OBJ_FLAG_HIDDEN);

    lbl_activity_waiting = lv_label_create(claude_activity_panel);
    lv_label_set_text(lbl_activity_waiting, "");
    lv_obj_set_width(lbl_activity_waiting, activity_content_width);
    lv_obj_set_style_text_font(lbl_activity_waiting, L.detail_font, 0);
    lv_obj_set_style_text_color(
        lbl_activity_waiting, lv_color_hex(ACTIVITY_WAITING_HEX), 0
    );
    lv_obj_set_pos(lbl_activity_waiting, 0, compact_activity ? 60 : 76);
    lv_obj_add_flag(lbl_activity_waiting, LV_OBJ_FLAG_HIDDEN);

    lbl_activity_codex_title = lv_label_create(codex_activity_panel);
    lv_label_set_text(lbl_activity_codex_title, "Codex");
    lv_obj_set_width(lbl_activity_codex_title, activity_content_width);
    lv_obj_set_style_text_font(lbl_activity_codex_title, L.detail_font, 0);
    lv_obj_set_style_text_color(lbl_activity_codex_title, COL_TEXT, 0);
    lv_obj_set_pos(lbl_activity_codex_title, 0, 0);

    lbl_activity_codex_unavailable = lv_label_create(codex_activity_panel);
    lv_label_set_text(lbl_activity_codex_unavailable, "Unavailable");
    lv_obj_set_width(lbl_activity_codex_unavailable, activity_content_width);
    lv_obj_set_style_text_font(lbl_activity_codex_unavailable, L.detail_font, 0);
    lv_obj_set_style_text_color(lbl_activity_codex_unavailable, COL_MUTED, 0);
    lv_obj_set_pos(
        lbl_activity_codex_unavailable, 0, compact_activity ? 28 : 36
    );

    lbl_activity_unread = lv_label_create(codex_activity_panel);
    lv_label_set_text(lbl_activity_unread, "");
    lv_obj_set_width(lbl_activity_unread, activity_content_width);
    lv_obj_set_style_text_font(lbl_activity_unread, L.detail_font, 0);
    lv_obj_set_style_text_color(
        lbl_activity_unread, lv_color_hex(ACTIVITY_UNREAD_HEX), 0
    );
    lv_obj_set_pos(lbl_activity_unread, 0, compact_activity ? 28 : 36);
    lv_obj_add_flag(lbl_activity_unread, LV_OBJ_FLAG_HIDDEN);

    activity_footer_label = lv_label_create(activity_container);
    lv_label_set_text(activity_footer_label, "Not scanned");
    lv_obj_set_width(activity_footer_label, L.content_w);
    lv_label_set_long_mode(activity_footer_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(activity_footer_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(activity_footer_label, L.detail_font, 0);
    lv_obj_set_style_text_color(activity_footer_label, COL_MUTED, 0);
    lv_obj_align(activity_footer_label, LV_ALIGN_TOP_MID, 0, L.footer_y);

    constexpr int DOT_SIZE = 5;
    constexpr int DOT_GAP = 8;
    constexpr int INDICATOR_WIDTH =
        (DASHBOARD_PAGE_COUNT * DOT_SIZE) + ((DASHBOARD_PAGE_COUNT - 1) * DOT_GAP);
    page_indicator_group = lv_obj_create(scr);
    lv_obj_set_size(page_indicator_group, INDICATOR_WIDTH, DOT_SIZE);
    lv_obj_set_pos(page_indicator_group, (L.scr_w - INDICATOR_WIDTH) / 2, L.page_indicator_y);
    lv_obj_set_style_bg_opa(page_indicator_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page_indicator_group, 0, 0);
    lv_obj_set_style_pad_all(page_indicator_group, 0, 0);
    lv_obj_clear_flag(page_indicator_group, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < DASHBOARD_PAGE_COUNT; ++i) {
        page_indicator_dots[i] = lv_obj_create(page_indicator_group);
        lv_obj_set_size(page_indicator_dots[i], DOT_SIZE, DOT_SIZE);
        lv_obj_set_pos(page_indicator_dots[i], i * (DOT_SIZE + DOT_GAP), 0);
        lv_obj_set_style_radius(page_indicator_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(page_indicator_dots[i], 0, 0);
        lv_obj_set_style_bg_opa(page_indicator_dots[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(
            page_indicator_dots[i], i == DASHBOARD_CLAUDE ? COL_TEXT : COL_MUTED, 0
        );
        lv_obj_clear_flag(page_indicator_dots[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    claude_logo_img = lv_image_create(scr);
    lv_image_set_src(claude_logo_img, &claude_logo_dsc);
    lv_image_set_pivot(claude_logo_img, 0, 0);
    lv_image_set_scale(claude_logo_img, L.logo_scale);
    lv_obj_set_pos(
        claude_logo_img,
        L.margin,
        (L.horizontal_cards || L.logo_size == 48) ? 6 : L.title_y - 10
    );

    codex_logo_img = lv_image_create(scr);
    lv_image_set_src(codex_logo_img, &codex_logo_dsc);
    lv_image_set_pivot(codex_logo_img, 0, 0);
    lv_image_set_scale(codex_logo_img, L.logo_scale);
    lv_obj_set_pos(
        codex_logo_img,
        L.margin,
        (L.horizontal_cards || L.logo_size == 48) ? 6 : L.title_y - 10
    );

    if (board_caps().has_battery) {
        init_battery_icons();
        battery_img = lv_image_create(scr);
        lv_image_set_src(battery_img, &battery_dscs[0]);
        const int16_t battery_width =
            (L.scr_w <= 240 && L.scr_h <= 240) ? ICON_BATTERY_SMALL_W : ICON_BATTERY_W;
        lv_obj_set_pos(battery_img, L.scr_w - battery_width - L.margin, L.title_y);
    }

    navigation_layer = lv_obj_create(scr);
    lv_obj_set_size(navigation_layer, L.scr_w, L.scr_h);
    lv_obj_set_pos(navigation_layer, 0, 0);
    lv_obj_set_style_bg_opa(navigation_layer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(navigation_layer, 0, 0);
    lv_obj_set_style_pad_all(navigation_layer, 0, 0);
    lv_obj_clear_flag(navigation_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(navigation_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(navigation_layer, global_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_move_foreground(navigation_layer);
}

void ui_update(const UsageData* data, uint8_t updates) {
    if (!data) return;

    activity_freshness_apply(activity_freshness, updates, millis());

    if (updates & DASHBOARD_UPDATE_CODEX) {
        if (data->codex.valid) {
            const uint8_t limit_count = data->codex.limit_count > 2
                ? 2
                : data->codex.limit_count;
            char buf[48];

            for (uint8_t i = 0; i < 2; ++i) {
                if (i < limit_count) {
                    const CodexLimitData& limit = data->codex.limits[i];
                    const int percent = static_cast<int>(limit.percent + 0.5f);
                    lv_label_set_text_fmt(codex_pct[i], "%d%%", percent);
                    lv_label_set_text(codex_label[i], codex_window_label(limit.window_mins));
                    lv_obj_clear_flag(codex_label[i], LV_OBJ_FLAG_HIDDEN);
                    lv_bar_set_value(codex_bar[i], percent, LV_ANIM_ON);
                    lv_obj_set_style_bg_color(
                        codex_bar[i], pct_color(limit.percent), LV_PART_INDICATOR
                    );
                    lv_obj_clear_flag(codex_bar[i], LV_OBJ_FLAG_HIDDEN);
                    format_reset_time(limit.reset_mins, buf, sizeof(buf));
                    lv_label_set_text(codex_reset[i], buf);
                } else if (i == 0) {
                    lv_label_set_text(codex_pct[i], "Codex");
                    lv_obj_add_flag(codex_label[i], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_add_flag(codex_bar[i], LV_OBJ_FLAG_HIDDEN);
                    lv_label_set_text(codex_reset[i], "No limit data");
                } else {
                    format_compact_tokens(data->codex.tokens_today, buf, sizeof(buf));
                    lv_label_set_text(codex_pct[i], buf);
                    lv_label_set_text(codex_label[i], "Tokens today");
                    lv_obj_clear_flag(codex_label[i], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_add_flag(codex_bar[i], LV_OBJ_FLAG_HIDDEN);
                    lv_label_set_text(codex_reset[i], "");
                }
            }
        } else {
            lv_label_set_text(codex_pct[0], "Codex");
            lv_label_set_text(codex_reset[0], "Unavailable");
            lv_obj_add_flag(codex_label[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(codex_bar[0], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(codex_pct[1], "---");
            lv_label_set_text(codex_label[1], "Tokens today");
            lv_obj_clear_flag(codex_label[1], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(codex_reset[1], "Unavailable");
            lv_obj_add_flag(codex_bar[1], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (updates & DASHBOARD_UPDATE_ACTIVITY) {
        if (data->activity.claude_valid) {
            lv_label_set_text_fmt(lbl_activity_open, "Open  %d", data->activity.claude_open);
            lv_label_set_text_fmt(lbl_activity_busy, "Busy  %d", data->activity.claude_busy);
            lv_label_set_text_fmt(lbl_activity_waiting, "Waiting  %d", data->activity.claude_waiting);
            lv_obj_add_flag(lbl_activity_claude_unavailable, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lbl_activity_open, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lbl_activity_busy, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lbl_activity_waiting, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lbl_activity_open, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lbl_activity_busy, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lbl_activity_waiting, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lbl_activity_claude_unavailable, LV_OBJ_FLAG_HIDDEN);
        }

        if (data->activity.codex_valid) {
            lv_label_set_text_fmt(lbl_activity_unread, "Unread  %d", data->activity.codex_unread);
            lv_obj_add_flag(lbl_activity_codex_unavailable, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lbl_activity_unread, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lbl_activity_unread, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lbl_activity_codex_unavailable, LV_OBJ_FLAG_HIDDEN);
        }

        char footer[32];
        format_activity_freshness(
            activity_freshness, millis(), footer, sizeof(footer)
        );
        lv_label_set_text(activity_footer_label, footer);
    }

    if (!(updates & DASHBOARD_UPDATE_CLAUDE) || !data->valid) return;
    last_data_ms = lv_tick_get();   // a valid usage update just landed → dot goes green
    data_received = true;

    int s_pct = (int)(data->session_pct + 0.5f);

    if (data->enterprise) {
        // Spending box: big number-only label + small "%" symbol + desc + pace
        lv_obj_set_style_text_font(
            lbl_session_pct,
            L.claude_compact_rows
                ? &font_styrene_16
                : (L.horizontal_cards ? L.percentage_font : L.title_font),
            0
        );
        lv_label_set_text(lbl_session_label, "Spending");
        lv_label_set_text(
            lbl_spending_desc,
            L.horizontal_cards ? "monthly budget" : "of your monthly budget"
        );
        if (L.claude_compact_rows) {
            lv_obj_clear_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lbl_spending_desc, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lbl_spending_desc, LV_OBJ_FLAG_HIDDEN);
        }
        if (L.horizontal_cards && !L.claude_compact_rows) {
            lv_obj_clear_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);
        } else if (!L.claude_compact_rows) {
            lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);
        }
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_style_text_font(
            lbl_session_pct,
            L.claude_compact_rows ? &font_styrene_16 : L.percentage_font,
            0
        );
        lv_label_set_text(
            lbl_session_label,
            L.claude_compact_rows ? "Currently" : "Current"
        );
        lv_obj_clear_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
    }

    char buf[48];

    // Pace vars used in both enterprise blocks below
    const char* pace_text = "Under pace";
    lv_color_t  pace_color = COL_GREEN;
    const char* pace_hex   = "788c5d";   // matches THEME_GREEN
    if (data->session_pct > (float)data->time_pct + 15.0f) {
        pace_text = "Over pace";  pace_color = COL_RED;   pace_hex = "c0392b";
    } else if (data->session_pct > (float)data->time_pct - 15.0f) {
        pace_text = "On pace";    pace_color = COL_AMBER; pace_hex = "d97757";
    }

    if (data->enterprise) {
        lv_label_set_text_fmt(
            lbl_session_pct,
            L.claude_compact_rows ? "%d%%" : "%d",
            s_pct
        );
        if (!L.claude_compact_rows) {
            lv_obj_align_to(lbl_session_pct_sym, lbl_session_pct,
                            LV_ALIGN_OUT_RIGHT_TOP, 4, 12);
        } else {
            lv_label_set_text(
                lbl_session_reset,
                data->reset_date[0] ? data->reset_date : "---"
            );
        }
        if (L.horizontal_cards && !L.claude_compact_rows) {
            lv_label_set_text(lbl_spending_status, pace_text);
            lv_obj_set_style_text_color(lbl_spending_status, pace_color, 0);
        }
    } else {
        lv_label_set_text_fmt(lbl_session_pct, "%d%%", s_pct);
        if (L.claude_compact_rows) {
            format_compact_reset_time(data->session_reset_mins, buf, sizeof(buf));
        } else {
            format_reset_time(data->session_reset_mins, buf, sizeof(buf));
        }
        lv_label_set_text(lbl_session_reset, buf);
    }

    lv_bar_set_value(bar_session, s_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_session, pct_color(data->session_pct), LV_PART_INDICATOR);

    if (data->enterprise) {
        // Period box: time % + dynamic pace color + "Resets <date>" label
        lv_label_set_text(lbl_weekly_label, "Period");
        lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", data->time_pct);
        lv_bar_set_value(bar_weekly, data->time_pct, LV_ANIM_ON);
        lv_color_t bar_pace = (data->session_pct <= (float)data->time_pct) ? COL_GREEN :
                              (data->session_pct <= (float)data->time_pct + 15.0f) ? COL_AMBER :
                              COL_RED;
        lv_obj_set_style_bg_color(bar_weekly, bar_pace, LV_PART_INDICATOR);
        if (L.claude_compact_rows) {
            snprintf(buf, sizeof(buf), "%s", data->reset_date);
        } else if (L.horizontal_cards) {
            snprintf(buf, sizeof(buf), "Resets %s", data->reset_date);
        } else {
            snprintf(buf, sizeof(buf), "#%s %s# - #faf9f5 Resets %s#",
                     pace_hex, pace_text, data->reset_date);
        }
        lv_label_set_text(lbl_weekly_reset, buf);
    } else {
        int w_pct = (int)(data->weekly_pct + 0.5f);
        lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", w_pct);
        lv_bar_set_value(bar_weekly, w_pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(bar_weekly, pct_color(data->weekly_pct), LV_PART_INDICATOR);
        if (L.claude_compact_rows) {
            format_compact_reset_time(data->weekly_reset_mins, buf, sizeof(buf));
        } else {
            format_reset_time(data->weekly_reset_mins, buf, sizeof(buf));
        }
        lv_label_set_text(lbl_weekly_reset, buf);
    }

    if (panel_fable) {
        if (data->fable_valid) {
            const int f_pct = static_cast<int>(data->fable_pct + 0.5f);
            lv_label_set_text_fmt(lbl_fable_pct, "%d%%", f_pct);
            format_compact_reset_time(
                data->fable_reset_mins, buf, sizeof(buf)
            );
            lv_label_set_text(lbl_fable_reset, buf);
            lv_bar_set_value(bar_fable, f_pct, LV_ANIM_ON);
            lv_obj_set_style_bg_color(
                bar_fable, pct_color(data->fable_pct), LV_PART_INDICATOR
            );
        } else {
            lv_label_set_text(lbl_fable_pct, "--");
            lv_label_set_text(lbl_fable_reset, "Unavailable");
            lv_bar_set_value(bar_fable, 0, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(
                bar_fable, COL_MUTED, LV_PART_INDICATOR
            );
        }
    }
}

// Pick the usage-view sub-screen: pairing hint (BLE down), the idle "Zzz" screen
// (connected but data has gone stale), or the live usage panels. Only re-lays-out
// on an actual change. The animated status line stays visible everywhere — it
// reads "Listening…" on the idle screen, keeping it alive rather than frozen.
static void update_view_state(void) {
    if (!usage_group || !pair_group || !idle_group) return;
    int v = static_cast<int>(select_usage_view_state(
        s_ble_connected,
        data_received,
        lv_tick_get(),
        last_data_ms,
        DATA_FRESH_MS
    ));
    if (v == view_state) return;
    view_state = v;
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(v == 0 ? pair_group : v == 1 ? idle_group : usage_group,
                      LV_OBJ_FLAG_HIDDEN);
}

void ui_tick_anim(void) {
    const uint32_t uptime_now = millis();
    if (current_page == DASHBOARD_ACTIVITY && activity_footer_label &&
        uptime_now - last_activity_footer_refresh_ms >= 1000) {
        last_activity_footer_refresh_ms = uptime_now;
        char footer[32];
        format_activity_freshness(
            activity_freshness, uptime_now, footer, sizeof(footer)
        );
        lv_label_set_text(activity_footer_label, footer);
    }

    if (current_page != DASHBOARD_CLAUDE) return;
    update_view_state();
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
    if (view_state == USAGE_VIEW_LIVE && !s_ble_connected) {
        text = (now - last_data_ms < 5000)
            ? "Updated"
            : anim_messages[anim_msg_idx];
    } else if (!s_ble_connected) {
        text = "Waiting";              // advertising / waiting for a host connection
    } else if (view_state == 1) {      // idle — alternate so it reads as alive AND data-less
        text = (anim_msg_idx & 1) ? "No data" : "Listening";
    } else if (now - connected_at_ms < 5000) {
        text = "Connected";
    } else {
        text = anim_messages[anim_msg_idx];
    }

    if (L.claude_compact_rows) {
        lv_label_set_text(lbl_anim, text);
    } else {
        static char buf[80];
        snprintf(
            buf,
            sizeof(buf),
            "%s %s\xE2\x80\xA6",
            spinner_frames[anim_spinner_idx],
            text
        );
        lv_label_set_text(lbl_anim, buf);
    }
}

static void apply_battery_visibility(void) {
    if (!battery_img) return;
    if (dashboard_battery_visible(dashboard_visibility, current_page)) {
        lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    }
}

static void apply_brand_visibility(DashboardPage page) {
    const uint8_t mask = dashboard_brand_mask(page);
    const bool activity = page == DASHBOARD_ACTIVITY;
    lv_obj_set_pos(
        codex_logo_img,
        activity ? L.scr_w - L.margin - L.logo_rendered_width : L.margin,
        (L.horizontal_cards || L.logo_size == 48) ? 6 : L.title_y - 10
    );
    if (mask & DASHBOARD_BRAND_CLAUDE) {
        lv_obj_clear_flag(claude_logo_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(claude_logo_img, LV_OBJ_FLAG_HIDDEN);
    }
    if (mask & DASHBOARD_BRAND_CODEX) {
        lv_obj_clear_flag(codex_logo_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(codex_logo_img, LV_OBJ_FLAG_HIDDEN);
    }
}

static void global_click_cb(lv_event_t* e) {
    (void)e;
    lv_indev_t* indev = lv_indev_active();
    if (!indev) return;

    lv_point_t point{};
    lv_indev_get_point(indev, &point);
    const uint32_t now = millis();
    if (!carousel.started) carousel_start(carousel, current_page, now);

    const DashboardNavigationDirection direction = dashboard_direction_for_x(
        static_cast<uint16_t>(point.x < 0 ? 0 : point.x),
        static_cast<uint16_t>(L.scr_w)
    );
    const DashboardPage page = direction == DASHBOARD_NAV_PREVIOUS
        ? carousel_manual_previous(carousel, now)
        : carousel_manual_next(carousel, now);
    ui_show_screen(page);
}

void ui_show_boot_splash(void) {
    dashboard_visibility_show_boot_splash(dashboard_visibility);
    lv_obj_add_flag(claude_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(codex_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(activity_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(claude_logo_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(codex_logo_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(page_indicator_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(navigation_layer, LV_OBJ_FLAG_HIDDEN);
    if (battery_img) lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    splash_show();
}

void ui_show_screen(DashboardPage page) {
    dashboard_visibility_show_dashboard(dashboard_visibility);
    lv_obj_add_flag(claude_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(codex_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(activity_container, LV_OBJ_FLAG_HIDDEN);
    splash_hide();

    switch (page) {
    case DASHBOARD_CLAUDE:
        lv_obj_clear_flag(claude_container, LV_OBJ_FLAG_HIDDEN);
        break;
    case DASHBOARD_CODEX:
        lv_obj_clear_flag(codex_container, LV_OBJ_FLAG_HIDDEN);
        break;
    case DASHBOARD_ACTIVITY:
        lv_obj_clear_flag(activity_container, LV_OBJ_FLAG_HIDDEN);
        break;
    default:
        break;
    }

    apply_brand_visibility(page);

    current_page = page;
    if (page_indicator_group) {
        lv_obj_clear_flag(page_indicator_group, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < DASHBOARD_PAGE_COUNT; ++i) {
            lv_obj_set_style_bg_color(
                page_indicator_dots[i],
                i == static_cast<int>(page) ? COL_TEXT : COL_MUTED,
                0
            );
        }
    }
    apply_battery_visibility();
    lv_obj_move_foreground(claude_logo_img);
    lv_obj_move_foreground(codex_logo_img);
    if (navigation_layer) {
        lv_obj_clear_flag(navigation_layer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(navigation_layer);
    }
}

void ui_start_dashboard(uint32_t now_ms) {
    carousel_start(carousel, DASHBOARD_CLAUDE, now_ms);
    ui_show_screen(DASHBOARD_CLAUDE);
}

void ui_tick_navigation(uint32_t now_ms) {
    if (carousel_tick(carousel, now_ms)) ui_show_screen(carousel.page);
}

DashboardPage ui_get_current_screen(void) {
    return current_page;
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
    if (!battery_img) return;

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
