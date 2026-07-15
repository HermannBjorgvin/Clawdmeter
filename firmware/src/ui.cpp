#include "ui.h"
#include "splash.h"
#include <lvgl.h>
#include <time.h>
#include "logo.h"
#include "codex_logo.h"
#include "gemini_logo.h"
#include "icons.h"
#include "hal/board_caps.h"
#include "hal/imu_hal.h"   // rotation quadrant — gestures arrive in native axes

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

    // Usage screen. Claude and Codex each get their own tab, so panels stay
    // full-size on both — no cramped multi-provider geometry.
    int16_t usage_panel_h;
    int16_t usage_panel_gap;
    int16_t usage_bar_y;
    int16_t usage_reset_y;
    int16_t usage_bar_h;
    int16_t subtitle_y;
    const lv_font_t* usage_pct_font;
    const lv_font_t* usage_reset_font;
    const lv_font_t* usage_pill_font;
    const lv_font_t* subtitle_font;

    // System screen
    int16_t system_bar_w;
    int16_t system_bar_h;
    int16_t system_bar_y;
    int16_t system_pct_y;
    int16_t system_name_y;
    int16_t system_temp_y;
    const lv_font_t* system_pct_font;
    const lv_font_t* system_name_font;
    const lv_font_t* system_temp_font;

    // Stats screen
    int16_t heat_y, heat_cell, heat_gap;
    int16_t stats_cap_dy;      // caption offset below its big number
    int16_t stats_model_dy;    // model block offset below the token block
    int16_t stats_row_dy;      // gap between the two stat rows

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
    // Title is font_tiempos_56 (line_height 58) on every board, so it spans
    // title_y .. title_y+58. The plan subtitle sits directly under that, and
    // content_y must clear the subtitle — keep these three in step.
    L.title_y = 22;

    if (c.height >= 460) {
        // Large layout — tuned for 480x480 (AMOLED-2.16).
        // 104 + 2*150 + 16 = 420, clearing the status line at y>=431.
        L.content_y = 104;
        L.usage_panel_h = 150;
        L.usage_panel_gap = 16;
        L.usage_bar_y = 56;
        L.usage_reset_y = 94;
        L.usage_bar_h = 24;
        L.usage_pct_font   = &font_styrene_48;
        L.usage_reset_font = &font_styrene_24;
        L.usage_pill_font  = &font_styrene_28;
        L.subtitle_y       = 80;   // title ends at 22+58=80
        L.subtitle_font    = &font_styrene_20;
        L.system_bar_w     = 72;
        L.system_bar_h     = 202;
        L.system_bar_y     = 154;
        L.system_pct_y     = 94;
        L.system_name_y    = 368;
        L.system_temp_y    = 398;
        L.system_pct_font  = &font_mono_32;
        L.system_name_font = &font_styrene_28;
        L.system_temp_font = &font_styrene_20;
        // Stats: 7x7 grid of 24px cells + 5 gap = 198 wide, leaving ~240 to its right.
        L.heat_y          = 104;
        L.heat_cell       = 24;
        L.heat_gap        = 5;
        L.stats_cap_dy    = 52;
        L.stats_model_dy  = 96;
        L.stats_row_dy    = 34;
        L.bt_info_panel_h = 160;
        L.bt_reset_zone_h = 110;
        L.bt_title_font    = &font_tiempos_56;
        L.bt_status_font   = &font_styrene_48;
        L.bt_device_font   = &font_styrene_28;
        L.bt_credit_1_font = &font_styrene_24;
        L.bt_credit_2_font = &font_styrene_20;
    } else {
        // Compact layout — tuned for 368x448 (AMOLED-1.8).
        // Title is the same 58px face here, so content starts below the subtitle.
        L.content_y = 100;
        L.usage_panel_h = 130;
        L.usage_panel_gap = 12;
        L.usage_bar_y = 48;
        L.usage_reset_y = 78;
        L.usage_bar_h = 24;
        L.usage_pct_font   = &font_styrene_48;
        L.usage_reset_font = &font_styrene_20;
        L.usage_pill_font  = &font_styrene_28;
        L.subtitle_y       = 80;   // title ends at 22+58=80
        L.subtitle_font    = &font_styrene_16;
        L.system_bar_w     = 60;
        L.system_bar_h     = 176;
        L.system_bar_y     = 148;
        L.system_pct_y     = 90;
        L.system_name_y    = 336;
        L.system_temp_y    = 362;
        L.system_pct_font  = &font_mono_32;
        L.system_name_font = &font_styrene_20;
        L.system_temp_font = &font_styrene_16;
        L.heat_y          = 100;
        L.heat_cell       = 18;
        L.heat_gap        = 4;
        L.stats_cap_dy    = 40;
        L.stats_model_dy  = 74;
        L.stats_row_dy    = 28;
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

// ---- Usage screen widgets (single non-splash view) ----
static lv_obj_t* usage_container;
static lv_obj_t* lbl_title;
// Clock fed by the daemon: base epoch (local wall-clock seconds) + the lv_tick at
// which it landed, so the title ticks forward locally between 60s payloads.
static long     clock_base_epoch = 0;
static uint32_t clock_base_ms = 0;
static int      clock_fmt = 24;   // 12 or 24, set from the daemon payload
static int      clock_last_min = -1;   // last rendered minute; avoids redrawing the title every tick
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
// Codex tab — its own full-size panel, reached by swiping left from the Claude tab.
static lv_obj_t* codex_group = nullptr;   // Codex panel, shown on SCREEN_CODEX
static lv_obj_t* bar_codex = nullptr;
static lv_obj_t* lbl_codex_pct = nullptr;
static lv_obj_t* lbl_codex_label = nullptr;
static lv_obj_t* lbl_codex_reset = nullptr;
static lv_obj_t* bar_codex_context = nullptr;
static lv_obj_t* lbl_codex_context_pct = nullptr;
static lv_obj_t* lbl_codex_context_label = nullptr;
static lv_obj_t* lbl_codex_context_detail = nullptr;
static lv_obj_t* panel_codex_context = nullptr;
static lv_obj_t* panel_codex = nullptr;
static lv_obj_t* lbl_codex_none = nullptr;  // "No Codex data" when the daemon omits it
static lv_obj_t* lbl_subtitle = nullptr;    // plan line under the title, e.g. "Claude Max 20x"
static bool      s_codex_valid = false;     // last payload carried Codex data
static char      claude_plan_str[24] = "";  // "Claude Max 20x" — from the daemon
static char      codex_plan_str[24]  = "";  // "Codex Plus"     — from the daemon
// Antigravity tab — Gemini pool from agy's /usage quota summary.
static lv_obj_t* antigravity_group = nullptr;
static lv_obj_t* bar_antigravity_5h = nullptr;
static lv_obj_t* lbl_antigravity_5h_pct = nullptr;
static lv_obj_t* lbl_antigravity_5h_label = nullptr;
static lv_obj_t* lbl_antigravity_5h_reset = nullptr;
static lv_obj_t* bar_antigravity_weekly = nullptr;
static lv_obj_t* lbl_antigravity_weekly_pct = nullptr;
static lv_obj_t* lbl_antigravity_weekly_label = nullptr;
static lv_obj_t* lbl_antigravity_weekly_reset = nullptr;
static lv_obj_t* panel_antigravity_5h = nullptr;
static lv_obj_t* panel_antigravity_weekly = nullptr;
static lv_obj_t* lbl_antigravity_none = nullptr;
static char      antigravity_plan_str[24] = "";
// System tab — three vertical host resource meters, immediately left of Claude.
static lv_obj_t* system_group = nullptr;
static lv_obj_t* system_bars[3] = {};
static lv_obj_t* system_pcts[3] = {};
static lv_obj_t* system_temps[3] = {};
static lv_obj_t* system_names[3] = {};
static lv_obj_t* lbl_system_none = nullptr;
static void      apply_subtitle(void);
// Enterprise-only widgets inside panel_session
static lv_obj_t* lbl_session_pct_sym = nullptr;  // "%" in smaller font
static lv_obj_t* lbl_spending_desc = nullptr;     // "of your monthly budget"
static lv_obj_t* lbl_spending_status = nullptr;   // "Under pace" / "On pace" / "Over pace"
static lv_obj_t* lbl_anim;      // status line: connection state + whimsical idle

// ---- Battery indicator (shared, on top) ----
static lv_obj_t* battery_img;
static lv_obj_t* logo_img;
static lv_obj_t* system_icon = nullptr;
static lv_image_dsc_t battery_dscs[5];  // empty, low, medium, full, charging

// ---- Live-data freshness → which usage sub-view to show ----
// usage panels when data is flowing, an idle "Zzz" screen when the host is
// connected but no usage update landed within DATA_FRESH_MS, the pairing hint
// when BLE is down. Re-evaluated every loop in ui_tick_anim().
static lv_obj_t* idle_group;            // the "Zzz" idle screen
static uint32_t  last_data_ms = 0;      // lv_tick when the last valid usage update landed
static bool      data_received = false; // any valid update since boot
static int       view_state = -1;       // -1 unknown / 0 pair / 1 idle / 2 usage
static screen_t  view_tab = SCREEN_USAGE;  // tab the current view_state was laid out for
static const uint32_t DATA_FRESH_MS = 90000;  // usage counts as "live" within this window (daemon sends ~60s)

// ---- Stats screen (/stats view, reached by tapping the title) ----
#define HEAT_COLS 7
#define HEAT_ROWS 7
#define HEAT_CELLS (HEAT_COLS * HEAT_ROWS)
static lv_obj_t* stats_container = nullptr;
static lv_obj_t* lbl_stats_title = nullptr;
static lv_obj_t* heat_cells[HEAT_CELLS] = {};
static lv_obj_t* lbl_stats_tokens = nullptr;
static lv_obj_t* lbl_stats_tokens_cap = nullptr;
static lv_obj_t* lbl_stats_model = nullptr;
static lv_obj_t* lbl_stats_model_cap = nullptr;
static lv_obj_t* lbl_stats_l1 = nullptr;   // "331 sessions"
static lv_obj_t* lbl_stats_r1 = nullptr;   // "17d streak"
static lv_obj_t* lbl_stats_l2 = nullptr;   // "9d 22h longest"
static lv_obj_t* lbl_stats_r2 = nullptr;   // "29/47 days"
static lv_obj_t* lbl_stats_dune = nullptr;
static lv_obj_t* lbl_stats_none = nullptr; // "No stats yet"
static StatsData s_stats[3] = {};          // Claude, Codex, Antigravity
static int       stats_provider = 0;       // which one SCREEN_STATS is showing
static screen_t  stats_from = SCREEN_USAGE; // tab to return to

// ---- Shared ----
static lv_image_dsc_t logo_dsc;
static lv_image_dsc_t codex_logo_dsc;   // OpenAI mark, shown on the Codex tab
static lv_image_dsc_t gemini_logo_dsc;  // Gemini mark, shown on Antigravity
static screen_t current_screen = SCREEN_USAGE;
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

static lv_color_t temp_color(int temp_c) {
    if (temp_c < 0) return COL_DIM;
    if (temp_c >= 85) return COL_RED;
    if (temp_c >= 70) return COL_AMBER;
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

static void format_compact_count(long value, char* buf, size_t len) {
    if (value < 0) {
        snprintf(buf, len, "---");
    } else if (value < 1000) {
        snprintf(buf, len, "%ld", value);
    } else if (value < 1000000L) {
        snprintf(buf, len, "%.1fk", value / 1000.0);
    } else {
        snprintf(buf, len, "%.1fm", value / 1000000.0);
    }
}

static void format_context_detail(long tokens, long window, char* buf, size_t len) {
    char used[24];
    char cap[24];
    format_compact_count(tokens, used, sizeof(used));
    format_compact_count(window, cap, sizeof(cap));
    if (tokens < 0 || window <= 0) {
        snprintf(buf, len, "---");
    } else {
        snprintf(buf, len, "%s / %s", used, cap);
    }
}

// Forward decls — callbacks defined near ui_show_screen below
static void global_click_cb(lv_event_t* e);
static void global_gesture_cb(lv_event_t* e);
static void logo_click_cb(lv_event_t* e);
static void usage_title_cb(lv_event_t* e);
static void render_stats(void);

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

static lv_obj_t* make_pill(lv_obj_t* parent, const char* text, const lv_font_t* font) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
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

static void init_system_icon(lv_obj_t* parent) {
    system_icon = lv_obj_create(parent);
    lv_obj_set_size(system_icon, 48, 48);
    lv_obj_set_pos(system_icon, L.margin, L.title_y - 10);
    lv_obj_set_style_bg_opa(system_icon, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(system_icon, 0, 0);
    lv_obj_set_style_pad_all(system_icon, 0, 0);
    lv_obj_clear_flag(system_icon, LV_OBJ_FLAG_SCROLLABLE);

    struct BarSpec { int x; int y; int w; int h; };
    const BarSpec bars[] = {
        {  8, 26,  6,  8 },
        { 19, 18,  6, 16 },
        { 30, 30,  6,  4 },
    };
    for (const BarSpec& bar : bars) {
        lv_obj_t* b = lv_obj_create(system_icon);
        lv_obj_set_pos(b, bar.x, bar.y);
        lv_obj_set_size(b, bar.w, bar.h);
        lv_obj_set_style_bg_color(b, COL_TEXT, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_radius(b, 2, 0);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_t* base = lv_obj_create(system_icon);
    lv_obj_set_pos(base, 10, 38);
    lv_obj_set_size(base, 28, 3);
    lv_obj_set_style_bg_color(base, COL_TEXT, 0);
    lv_obj_set_style_bg_opa(base, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(base, 0, 0);
    lv_obj_set_style_radius(base, 2, 0);
    lv_obj_clear_flag(base, LV_OBJ_FLAG_SCROLLABLE);
}

// ======== Usage Screen ========

static lv_obj_t* make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                                  lv_obj_t** out_pct, lv_obj_t** out_pill,
                                  lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, L.margin, y, L.content_w, L.usage_panel_h);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, L.usage_pct_font, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text, L.usage_pill_font);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, L.usage_bar_y, L.content_w - 32, L.usage_bar_h);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, L.usage_reset_font, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, L.usage_reset_y);

    return panel;
}

static void make_resource_meter(lv_obj_t* parent, int index, const char* name) {
    const int col_w = L.content_w / 3;
    const int center_x = L.margin + col_w * index + col_w / 2;
    const int bar_w = L.system_bar_w;
    const int bar_h = L.system_bar_h;
    const int bar_y = L.system_bar_y;

    system_pcts[index] = lv_label_create(parent);
    lv_label_set_text(system_pcts[index], "--%");
    lv_obj_set_style_text_font(system_pcts[index], L.system_pct_font, 0);
    lv_obj_set_style_text_color(system_pcts[index], COL_TEXT, 0);
    lv_obj_align(system_pcts[index], LV_ALIGN_TOP_MID,
                 center_x - L.scr_w / 2, L.system_pct_y);

    system_bars[index] = lv_bar_create(parent);
    lv_obj_set_pos(system_bars[index], center_x - bar_w / 2, bar_y);
    lv_obj_set_size(system_bars[index], bar_w, bar_h);
    lv_bar_set_range(system_bars[index], 0, 100);
    lv_bar_set_orientation(system_bars[index], LV_BAR_ORIENTATION_VERTICAL);
    lv_bar_set_value(system_bars[index], 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(system_bars[index], COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(system_bars[index], LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(system_bars[index], 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(system_bars[index], COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(system_bars[index], LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(system_bars[index], 8, LV_PART_INDICATOR);

    system_names[index] = lv_label_create(parent);
    lv_label_set_text(system_names[index], name);
    lv_obj_set_style_text_font(system_names[index], L.system_name_font, 0);
    lv_obj_set_style_text_color(system_names[index], COL_TEXT, 0);
    lv_obj_align(system_names[index], LV_ALIGN_TOP_MID,
                 center_x - L.scr_w / 2, L.system_name_y);

    system_temps[index] = lv_label_create(parent);
    lv_label_set_text(system_temps[index], index < 2 ? "-- C" : "");
    lv_obj_set_style_text_font(system_temps[index], L.system_temp_font, 0);
    lv_obj_set_style_text_color(system_temps[index], COL_DIM, 0);
    lv_obj_align(system_temps[index], LV_ALIGN_TOP_MID,
                 center_x - L.scr_w / 2, L.system_temp_y);
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
    // Labels aren't clickable by default — tapping the title opens /stats.
    lv_obj_add_flag(lbl_title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(lbl_title, usage_title_cb, LV_EVENT_CLICKED, NULL);

    // Plan line under the title ("Claude Max 20x" / "Codex Plus"). Filled from the
    // daemon payload; stays empty (and invisible) if it never sends one.
    lbl_subtitle = lv_label_create(usage_container);
    lv_label_set_text(lbl_subtitle, "");
    lv_obj_set_style_text_font(lbl_subtitle, L.subtitle_font, 0);
    lv_obj_set_style_text_color(lbl_subtitle, COL_DIM, 0);
    lv_obj_align(lbl_subtitle, LV_ALIGN_TOP_MID, 16, L.subtitle_y);

    // Usage panels (shown when connected) live in a transparent full-size group
    // so they can be toggled against the pairing hint as one unit.
    usage_group = lv_obj_create(usage_container);
    lv_obj_set_size(usage_group, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_group, 0, 0);
    lv_obj_set_style_bg_opa(usage_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_group, 0, 0);
    lv_obj_set_style_pad_all(usage_group, 0, 0);
    lv_obj_clear_flag(usage_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    panel_session = make_usage_panel(usage_group, L.content_y, "Current",
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset);

    // Enterprise-only overlays inside panel_session — hidden until enterprise data arrives
    lbl_session_pct_sym = lv_label_create(panel_session);
    lv_label_set_text(lbl_session_pct_sym, "%");
    lv_obj_set_style_text_font(lbl_session_pct_sym, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_session_pct_sym, COL_TEXT, 0);
    lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_desc = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_desc, "of your monthly budget");
    lv_obj_set_style_text_font(lbl_spending_desc, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_spending_desc, COL_DIM, 0);
    lv_obj_set_pos(lbl_spending_desc, 0, L.usage_reset_y);
    lv_obj_add_flag(lbl_spending_desc, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_status = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_status, "");
    lv_obj_set_style_text_font(lbl_spending_status, &font_styrene_16, 0);
    lv_obj_set_pos(lbl_spending_status, 0, L.usage_reset_y + 20);
    lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);

    panel_weekly = make_usage_panel(usage_group,
                     L.content_y + L.usage_panel_h + L.usage_panel_gap, "Weekly",
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset);
    // Recolor enabled so enterprise period box can color pace and reset separately
    lv_label_set_recolor(lbl_weekly_reset, true);

    // ---- Codex tab: its own group, same full-size panel geometry as Claude ----
    codex_group = lv_obj_create(usage_container);
    lv_obj_set_size(codex_group, L.scr_w, L.scr_h);
    lv_obj_set_pos(codex_group, 0, 0);
    lv_obj_set_style_bg_opa(codex_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(codex_group, 0, 0);
    lv_obj_set_style_pad_all(codex_group, 0, 0);
    lv_obj_clear_flag(codex_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(codex_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    panel_codex = make_usage_panel(codex_group, L.content_y, "Weekly",
                     &lbl_codex_pct, &lbl_codex_label,
                     &bar_codex, &lbl_codex_reset);
    panel_codex_context = make_usage_panel(codex_group,
                     L.content_y + L.usage_panel_h + L.usage_panel_gap, "Context",
                     &lbl_codex_context_pct, &lbl_codex_context_label,
                     &bar_codex_context, &lbl_codex_context_detail);

    // Shown instead of the panel when the daemon sends no Codex data.
    lbl_codex_none = lv_label_create(codex_group);
    lv_label_set_text(lbl_codex_none, "No Codex data");
    lv_obj_set_style_text_font(lbl_codex_none, L.usage_pill_font, 0);
    lv_obj_set_style_text_color(lbl_codex_none, COL_DIM, 0);
    lv_obj_align(lbl_codex_none, LV_ALIGN_CENTER, 0, -20);
    lv_obj_add_flag(lbl_codex_none, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(codex_group, LV_OBJ_FLAG_HIDDEN);   // update_view_state decides

    // ---- Antigravity CLI tab: Gemini's 5-hour and weekly quota buckets ----
    antigravity_group = lv_obj_create(usage_container);
    lv_obj_set_size(antigravity_group, L.scr_w, L.scr_h);
    lv_obj_set_pos(antigravity_group, 0, 0);
    lv_obj_set_style_bg_opa(antigravity_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(antigravity_group, 0, 0);
    lv_obj_set_style_pad_all(antigravity_group, 0, 0);
    lv_obj_clear_flag(antigravity_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(antigravity_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    panel_antigravity_5h = make_usage_panel(antigravity_group, L.content_y, "Current",
                     &lbl_antigravity_5h_pct, &lbl_antigravity_5h_label,
                     &bar_antigravity_5h, &lbl_antigravity_5h_reset);
    panel_antigravity_weekly = make_usage_panel(antigravity_group,
                     L.content_y + L.usage_panel_h + L.usage_panel_gap, "Weekly",
                     &lbl_antigravity_weekly_pct, &lbl_antigravity_weekly_label,
                     &bar_antigravity_weekly, &lbl_antigravity_weekly_reset);

    lbl_antigravity_none = lv_label_create(antigravity_group);
    lv_label_set_text(lbl_antigravity_none, "No Antigravity data");
    lv_obj_set_style_text_font(lbl_antigravity_none, L.usage_pill_font, 0);
    lv_obj_set_style_text_color(lbl_antigravity_none, COL_DIM, 0);
    lv_obj_align(lbl_antigravity_none, LV_ALIGN_CENTER, 0, -20);
    lv_obj_add_flag(lbl_antigravity_none, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(antigravity_group, LV_OBJ_FLAG_HIDDEN);

    system_group = lv_obj_create(usage_container);
    lv_obj_set_size(system_group, L.scr_w, L.scr_h);
    lv_obj_set_pos(system_group, 0, 0);
    lv_obj_set_style_bg_opa(system_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(system_group, 0, 0);
    lv_obj_set_style_pad_all(system_group, 0, 0);
    lv_obj_clear_flag(system_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(system_group, LV_OBJ_FLAG_EVENT_BUBBLE);
    make_resource_meter(system_group, 0, "CPU");
    make_resource_meter(system_group, 1, "GPU");
    make_resource_meter(system_group, 2, "RAM");
    lbl_system_none = lv_label_create(system_group);
    lv_label_set_text(lbl_system_none, "No system data");
    lv_obj_set_style_text_font(lbl_system_none, L.usage_pill_font, 0);
    lv_obj_set_style_text_color(lbl_system_none, COL_DIM, 0);
    lv_obj_align(lbl_system_none, LV_ALIGN_CENTER, 0, -20);
    lv_obj_add_flag(lbl_system_none, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(system_group, LV_OBJ_FLAG_HIDDEN);

    build_pair_group(usage_container);
    build_idle_group(usage_container);

    // The four groups above are full-screen transparent layout wrappers, created
    // AFTER lbl_title — and lv_obj_create() flags every object CLICKABLE by
    // default (lv_obj.c). So they sat on top of the title and swallowed its taps:
    // the title still rendered (they're transparent) but never got
    // LV_EVENT_CLICKED. They are not interactive, so take them out of hit-testing
    // entirely, and lift the header above them so nothing can bury it again.
    lv_obj_t* const wrappers[] = { system_group, usage_group, codex_group, antigravity_group,
                                   pair_group, idle_group };
    for (unsigned i = 0; i < sizeof(wrappers) / sizeof(wrappers[0]); i++) {
        if (wrappers[i]) lv_obj_clear_flag(wrappers[i], LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_move_foreground(lbl_title);
    if (lbl_subtitle) lv_obj_move_foreground(lbl_subtitle);

    // Status line — always visible on the usage view. Driven by ui_tick_anim().
    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, &font_mono_32, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -15);
}

// Heatmap intensity ramp: 0 = no activity (bar background), then dark green up
// through the accent orange, matching the usage bars' own palette.
static lv_color_t heat_color(char c) {
    switch (c) {
    case '1': return lv_color_hex(0x39452c);
    case '2': return COL_GREEN;
    case '3': return COL_AMBER;
    case '4': return COL_ACCENT;
    default:  return COL_BAR_BG;
    }
}

// "857580" -> "9d 22h"; "2870" -> "47m". Compact enough for a stat line.
static void format_duration(long secs, char* buf, size_t len) {
    if (secs <= 0)          snprintf(buf, len, "--");
    else if (secs < 3600)   snprintf(buf, len, "%ldm", secs / 60);
    else if (secs < 86400)  snprintf(buf, len, "%ldh %ldm", secs / 3600, (secs % 3600) / 60);
    else                    snprintf(buf, len, "%ldd %ldh", secs / 86400, (secs % 86400) / 3600);
}

static void stats_title_cb(lv_event_t* e);

static void init_stats_screen(lv_obj_t* scr) {
    stats_container = lv_obj_create(scr);
    lv_obj_set_size(stats_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(stats_container, 0, 0);
    lv_obj_set_style_bg_opa(stats_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(stats_container, 0, 0);
    lv_obj_set_style_pad_all(stats_container, 0, 0);
    lv_obj_clear_flag(stats_container, LV_OBJ_FLAG_SCROLLABLE);

    lbl_stats_title = lv_label_create(stats_container);
    lv_label_set_text(lbl_stats_title, "Stats");
    lv_obj_set_style_text_font(lbl_stats_title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(lbl_stats_title, COL_TEXT, 0);
    lv_obj_align(lbl_stats_title, LV_ALIGN_TOP_MID, 16, L.title_y);
    // Labels aren't clickable by default — tapping the title pages back.
    lv_obj_add_flag(lbl_stats_title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(lbl_stats_title, stats_title_cb, LV_EVENT_CLICKED, NULL);

    const int cell = L.heat_cell;
    const int gap  = L.heat_gap;
    for (int i = 0; i < HEAT_CELLS; i++) {
        lv_obj_t* c = lv_obj_create(stats_container);
        lv_obj_set_size(c, cell, cell);
        lv_obj_set_pos(c, L.margin + (i % HEAT_COLS) * (cell + gap),
                          L.heat_y  + (i / HEAT_COLS) * (cell + gap));
        lv_obj_set_style_radius(c, 3, 0);
        lv_obj_set_style_border_width(c, 0, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(c, COL_BAR_BG, 0);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        heat_cells[i] = c;
    }

    const int rx = L.margin + HEAT_COLS * (cell + gap) + 16;   // right of the grid
    lbl_stats_tokens = lv_label_create(stats_container);
    lv_obj_set_style_text_font(lbl_stats_tokens, L.usage_pct_font, 0);
    lv_obj_set_style_text_color(lbl_stats_tokens, COL_TEXT, 0);
    lv_obj_set_pos(lbl_stats_tokens, rx, L.heat_y);
    lv_label_set_text(lbl_stats_tokens, "--");

    lbl_stats_tokens_cap = lv_label_create(stats_container);
    lv_obj_set_style_text_font(lbl_stats_tokens_cap, L.subtitle_font, 0);
    lv_obj_set_style_text_color(lbl_stats_tokens_cap, COL_DIM, 0);
    lv_obj_set_pos(lbl_stats_tokens_cap, rx, L.heat_y + L.stats_cap_dy);
    lv_label_set_text(lbl_stats_tokens_cap, "tokens");

    lbl_stats_model = lv_label_create(stats_container);
    lv_obj_set_style_text_font(lbl_stats_model, L.usage_reset_font, 0);
    lv_obj_set_style_text_color(lbl_stats_model, COL_TEXT, 0);
    lv_obj_set_pos(lbl_stats_model, rx, L.heat_y + L.stats_model_dy);
    lv_label_set_text(lbl_stats_model, "--");

    lbl_stats_model_cap = lv_label_create(stats_container);
    lv_obj_set_style_text_font(lbl_stats_model_cap, L.subtitle_font, 0);
    lv_obj_set_style_text_color(lbl_stats_model_cap, COL_DIM, 0);
    lv_obj_set_pos(lbl_stats_model_cap, rx, L.heat_y + L.stats_model_dy + L.stats_cap_dy);
    lv_label_set_text(lbl_stats_model_cap, "favorite");

    const int row_y = L.heat_y + HEAT_ROWS * (cell + gap) + 14;
    lv_obj_t** rows[4] = { &lbl_stats_l1, &lbl_stats_r1, &lbl_stats_l2, &lbl_stats_r2 };
    for (int i = 0; i < 4; i++) {
        lv_obj_t* l = lv_label_create(stats_container);
        lv_obj_set_style_text_font(l, L.usage_reset_font, 0);
        lv_obj_set_style_text_color(l, COL_TEXT, 0);
        lv_obj_set_pos(l, (i & 1) ? L.scr_w / 2 + 8 : L.margin,
                          row_y + (i / 2) * L.stats_row_dy);
        lv_label_set_text(l, "");
        *rows[i] = l;
    }

    lbl_stats_dune = lv_label_create(stats_container);
    lv_obj_set_style_text_font(lbl_stats_dune, L.subtitle_font, 0);
    lv_obj_set_style_text_color(lbl_stats_dune, COL_ACCENT, 0);
    lv_obj_align(lbl_stats_dune, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_label_set_text(lbl_stats_dune, "");

    lbl_stats_none = lv_label_create(stats_container);
    lv_obj_set_style_text_font(lbl_stats_none, L.usage_pill_font, 0);
    lv_obj_set_style_text_color(lbl_stats_none, COL_DIM, 0);
    lv_label_set_text(lbl_stats_none, "No stats yet");
    lv_obj_align(lbl_stats_none, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(lbl_stats_none, LV_OBJ_FLAG_HIDDEN);

    // Same trap as the usage screen: every heat cell is a CLICKABLE lv_obj by
    // default. They don't overlap the title today, but keep the title on top so
    // a later layout tweak can't silently steal its taps.
    lv_obj_move_foreground(lbl_stats_title);

    lv_obj_add_flag(stats_container, LV_OBJ_FLAG_HIDDEN);
}

// Paint the stats screen from whichever provider the user came from.
static void render_stats(void) {
    if (!stats_container) return;
    const StatsData* s = &s_stats[(stats_provider >= 0 && stats_provider < 3)
                                  ? stats_provider : 0];
    const bool ok = s->valid;

    if (lbl_stats_none) {
        if (ok) lv_obj_add_flag(lbl_stats_none, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_clear_flag(lbl_stats_none, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_t* body[] = { lbl_stats_tokens, lbl_stats_tokens_cap, lbl_stats_model,
                         lbl_stats_model_cap, lbl_stats_l1, lbl_stats_r1,
                         lbl_stats_l2, lbl_stats_r2, lbl_stats_dune };
    for (unsigned i = 0; i < sizeof(body) / sizeof(body[0]); i++) {
        if (!body[i]) continue;
        if (ok) lv_obj_clear_flag(body[i], LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(body[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < HEAT_CELLS; i++) {
        if (!heat_cells[i]) continue;
        if (ok) lv_obj_clear_flag(heat_cells[i], LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(heat_cells[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (!ok) return;

    const int hlen = (int)strlen(s->heat);
    for (int i = 0; i < HEAT_CELLS; i++) {
        // heat is oldest->newest; a short/absent string just leaves cells empty.
        char c = (i < hlen) ? s->heat[i] : '0';
        lv_obj_set_style_bg_color(heat_cells[i], heat_color(c), 0);
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "%.1fm", s->total_tokens_m);
    lv_label_set_text(lbl_stats_tokens, buf);
    lv_label_set_text(lbl_stats_model, s->model[0] ? s->model : "--");

    lv_label_set_text_fmt(lbl_stats_l1, "%d sessions", s->sessions);
    lv_label_set_text_fmt(lbl_stats_r1, "%dd streak", s->streak);
    format_duration(s->longest_secs, buf, sizeof(buf));
    lv_label_set_text_fmt(lbl_stats_l2, "%s longest", buf);
    lv_label_set_text_fmt(lbl_stats_r2, "%d/%d days", s->active_days, s->span_days);
    lv_label_set_text_fmt(lbl_stats_dune, "~%dx more tokens than Dune", s->dune);
    lv_obj_align(lbl_stats_dune, LV_ALIGN_BOTTOM_MID, 0, -18);
}

void ui_update_stats(const StatsData* claude, const StatsData* codex,
                     const StatsData* antigravity) {
    if (claude) s_stats[0] = *claude;
    if (codex)  s_stats[1] = *codex;
    if (antigravity) s_stats[2] = *antigravity;
    if (current_screen == SCREEN_STATS) render_stats();
}

// ======== Public API ========

void ui_init(void) {
    compute_layout(board_caps());

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    init_icon_dsc_rgb565a8(&logo_dsc, LOGO_WIDTH, LOGO_HEIGHT, logo_data);
    init_icon_dsc_rgb565a8(&codex_logo_dsc, CODEX_LOGO_WIDTH, CODEX_LOGO_HEIGHT, codex_logo_data);
    init_icon_dsc_rgb565a8(&gemini_logo_dsc, GEMINI_LOGO_W, GEMINI_LOGO_H, gemini_logo);
    init_battery_icons();

    init_usage_screen(scr);
    init_stats_screen(scr);
    splash_init(scr);

    // Gesture handling lives on the SCREEN, not on usage_container. LVGL's
    // indev_gesture() walks up from the pressed object while each ancestor has
    // LV_OBJ_FLAG_GESTURE_BUBBLE — and lv_obj_create() sets that flag on every
    // object that has a parent (lv_obj.c). So the walk climbs past every
    // container and only stops at the screen, which has no parent and therefore
    // no flag. A handler anywhere below the screen is simply never called.
    lv_obj_add_event_cb(scr, global_gesture_cb, LV_EVENT_GESTURE, NULL);

    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, &logo_dsc);
    lv_obj_set_pos(logo_img, L.margin, L.title_y - 10);
    // The logo is now the ONLY way into the splash — images aren't clickable by
    // default, and it must sit above the containers to receive the tap.
    lv_obj_add_flag(logo_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(logo_img, logo_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_move_foreground(logo_img);

    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    lv_obj_set_pos(battery_img, L.scr_w - 48 - L.margin, L.title_y);
    init_system_icon(scr);

}

void ui_update(const UsageData* data) {
    if (!data->valid) return;
    last_data_ms = lv_tick_get();   // a valid usage update just landed → dot goes green
    data_received = true;

    if (data->clock_epoch > 0) {    // daemon supplied wall-clock time → drive the title clock
        clock_base_epoch = data->clock_epoch;
        clock_base_ms = last_data_ms;
        clock_fmt = data->clock_fmt;
    } else if (clock_base_epoch != 0) {   // clock turned off daemon-side → revert title to "Usage"
        clock_base_epoch = 0;
        clock_last_min = -1;
        lv_label_set_text(lbl_title, "Usage");
    }

    int s_pct = (int)(data->session_pct + 0.5f);

    // Codex is its own tab; Enterprise keeps its spending layout and gets none.
    const bool show_codex = data->codex_valid && !data->enterprise;
    s_codex_valid = show_codex;
    const bool show_antigravity = data->antigravity_valid;

    // Plan labels drive the subtitle on each tab.
    snprintf(claude_plan_str, sizeof(claude_plan_str), "%s",
             data->plan[0] ? data->plan : "");
    snprintf(codex_plan_str, sizeof(codex_plan_str), "%s",
             show_codex && data->codex_plan[0] ? data->codex_plan : "");
    snprintf(antigravity_plan_str, sizeof(antigravity_plan_str), "%s",
             show_antigravity && data->antigravity_plan[0]
                 ? data->antigravity_plan : "");
    apply_subtitle();

    if (panel_codex) {
        if (show_codex) { lv_obj_clear_flag(panel_codex, LV_OBJ_FLAG_HIDDEN);
                          if (lbl_codex_none) lv_obj_add_flag(lbl_codex_none, LV_OBJ_FLAG_HIDDEN); }
        else            { lv_obj_add_flag(panel_codex, LV_OBJ_FLAG_HIDDEN);
                          if (lbl_codex_none) lv_obj_clear_flag(lbl_codex_none, LV_OBJ_FLAG_HIDDEN); }
    }
    if (panel_codex_context) {
        if (show_codex && data->codex_context_valid) {
            lv_obj_clear_flag(panel_codex_context, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(panel_codex_context, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (panel_antigravity_5h && panel_antigravity_weekly) {
        if (show_antigravity) {
            lv_obj_clear_flag(panel_antigravity_5h, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(panel_antigravity_weekly, LV_OBJ_FLAG_HIDDEN);
            if (lbl_antigravity_none) lv_obj_add_flag(lbl_antigravity_none, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(panel_antigravity_5h, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(panel_antigravity_weekly, LV_OBJ_FLAG_HIDDEN);
            if (lbl_antigravity_none) lv_obj_clear_flag(lbl_antigravity_none, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (system_group) {
        for (int i = 0; i < 3; ++i) {
            lv_obj_t* widgets[] = { system_bars[i], system_pcts[i],
                                    system_temps[i], system_names[i] };
            for (lv_obj_t* widget : widgets) {
                if (!widget) continue;
                if (data->system_valid) lv_obj_clear_flag(widget, LV_OBJ_FLAG_HIDDEN);
                else                    lv_obj_add_flag(widget, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (lbl_system_none) {
            if (data->system_valid) lv_obj_add_flag(lbl_system_none, LV_OBJ_FLAG_HIDDEN);
            else                    lv_obj_clear_flag(lbl_system_none, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (data->enterprise) {
        // Spending box: big number-only label + small "%" symbol + desc + pace
        lv_obj_set_style_text_font(lbl_session_pct, &font_tiempos_56, 0);
        lv_label_set_text(lbl_session_label, "Spending");
        lv_obj_add_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status,   LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_style_text_font(lbl_session_pct, L.usage_pct_font, 0);
        lv_label_set_text(lbl_session_label, "Current");
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
        lv_label_set_text_fmt(lbl_session_pct, "%d", s_pct);
        lv_obj_align_to(lbl_session_pct_sym, lbl_session_pct,
                        LV_ALIGN_OUT_RIGHT_TOP, 4, 12);
    } else {
        lv_label_set_text_fmt(lbl_session_pct, "%d%%", s_pct);
        format_reset_time(data->session_reset_mins, buf, sizeof(buf));
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
        snprintf(buf, sizeof(buf), "#%s %s# - #faf9f5 Resets %s#",
                 pace_hex, pace_text, data->reset_date);
        lv_label_set_text(lbl_weekly_reset, buf);
    } else {
        int w_pct = (int)(data->weekly_pct + 0.5f);
        lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", w_pct);
        lv_bar_set_value(bar_weekly, w_pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(bar_weekly, pct_color(data->weekly_pct), LV_PART_INDICATOR);
        format_reset_time(data->weekly_reset_mins, buf, sizeof(buf));
        lv_label_set_text(lbl_weekly_reset, buf);
    }

    if (show_codex) {
        int c_pct = (int)(data->codex_pct + 0.5f);
        lv_label_set_text_fmt(lbl_codex_pct, "%d%%", c_pct);
        lv_bar_set_value(bar_codex, c_pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(bar_codex, pct_color(data->codex_pct), LV_PART_INDICATOR);
        format_reset_time(data->codex_reset_mins, buf, sizeof(buf));
        lv_label_set_text(lbl_codex_reset, buf);
        // Label the window from its actual length rather than assuming "weekly":
        // Plus exposes only a 7d window today, but the API models others.
        if (data->codex_window_mins >= 10080) {
            lv_label_set_text(lbl_codex_label, "Weekly");
        } else if (data->codex_window_mins >= 60) {
            lv_label_set_text_fmt(lbl_codex_label, "%dh", data->codex_window_mins / 60);
        } else {
            lv_label_set_text_fmt(lbl_codex_label, "%dm", data->codex_window_mins);
        }
        lv_obj_align(lbl_codex_label, LV_ALIGN_TOP_RIGHT, 0, 1);

        if (data->codex_context_valid) {
            int ctx_pct = (int)(((double)data->codex_context_tokens * 100.0) /
                                (double)data->codex_context_window + 0.5);
            if (ctx_pct < 0) ctx_pct = 0;
            if (ctx_pct > 100) ctx_pct = 100;
            char detail[48];
            lv_label_set_text_fmt(lbl_codex_context_pct, "%d%%", ctx_pct);
            lv_bar_set_value(bar_codex_context, ctx_pct, LV_ANIM_ON);
            lv_obj_set_style_bg_color(bar_codex_context,
                                      pct_color((float)ctx_pct), LV_PART_INDICATOR);
            format_context_detail(data->codex_context_tokens,
                                  data->codex_context_window, detail, sizeof(detail));
            lv_label_set_text(lbl_codex_context_detail, detail);
            lv_label_set_text(lbl_codex_context_label, "Context");
            lv_obj_align(lbl_codex_context_label, LV_ALIGN_TOP_RIGHT, 0, 1);
        }
    }

    if (show_antigravity) {
        const int five_pct = (int)(data->antigravity_5h_pct + 0.5f);
        const int week_pct = (int)(data->antigravity_weekly_pct + 0.5f);
        lv_label_set_text_fmt(lbl_antigravity_5h_pct, "%d%%", five_pct);
        lv_bar_set_value(bar_antigravity_5h, five_pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(bar_antigravity_5h,
                                  pct_color(data->antigravity_5h_pct), LV_PART_INDICATOR);
        format_reset_time(data->antigravity_5h_reset_mins, buf, sizeof(buf));
        lv_label_set_text(lbl_antigravity_5h_reset, buf);

        lv_label_set_text_fmt(lbl_antigravity_weekly_pct, "%d%%", week_pct);
        lv_bar_set_value(bar_antigravity_weekly, week_pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(bar_antigravity_weekly,
                                  pct_color(data->antigravity_weekly_pct), LV_PART_INDICATOR);
        format_reset_time(data->antigravity_weekly_reset_mins, buf, sizeof(buf));
        lv_label_set_text(lbl_antigravity_weekly_reset, buf);
    }

    if (data->system_valid) {
        const float values[] = { data->cpu_pct, data->gpu_pct, data->ram_pct };
        const int temps[] = { data->cpu_temp_c, data->gpu_temp_c, -1 };
        for (int i = 0; i < 3; ++i) {
            const int value = (int)(values[i] + 0.5f);
            lv_label_set_text_fmt(system_pcts[i], "%d%%", value);
            lv_bar_set_value(system_bars[i], value, LV_ANIM_ON);
            lv_obj_set_style_bg_color(system_bars[i], pct_color(values[i]), LV_PART_INDICATOR);
            if (i < 2) {
                if (temps[i] >= 0) lv_label_set_text_fmt(system_temps[i], "%d C", temps[i]);
                else               lv_label_set_text(system_temps[i], "No sensor");
                lv_obj_set_style_text_color(system_temps[i], temp_color(temps[i]), 0);
            }
        }
    }
}

// Pick the usage-view sub-screen: pairing hint (BLE down), the idle "Zzz" screen
// (connected but data has gone stale), or the live usage panels. Only re-lays-out
// on an actual change. The animated status line stays visible everywhere — it
// reads "Listening…" on the idle screen, keeping it alive rather than frozen.
static void update_view_state(void) {
    if (!usage_group || !pair_group || !idle_group) return;
    if (current_screen == SCREEN_SYSTEM) {
        view_state = 2;
        view_tab = current_screen;
        lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(usage_group, LV_OBJ_FLAG_HIDDEN);
        if (codex_group) lv_obj_add_flag(codex_group, LV_OBJ_FLAG_HIDDEN);
        if (antigravity_group) lv_obj_add_flag(antigravity_group, LV_OBJ_FLAG_HIDDEN);
        if (system_group) lv_obj_clear_flag(system_group, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    int v;
    if (!s_ble_connected) {
        v = 0;  // pairing hint
    } else if (data_received && (lv_tick_get() - last_data_ms) < DATA_FRESH_MS) {
        v = 2;  // live usage
    } else {
        v = 1;  // idle / Zzz
    }
    // The live view differs per tab, so the cache key is (freshness, tab) — not
    // freshness alone. ui_show_screen() resets view_state to force a re-lay-out.
    if (v == view_state && view_tab == current_screen) return;
    view_state = v;
    view_tab = current_screen;
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_HIDDEN);
    if (codex_group) lv_obj_add_flag(codex_group, LV_OBJ_FLAG_HIDDEN);
    if (antigravity_group) lv_obj_add_flag(antigravity_group, LV_OBJ_FLAG_HIDDEN);
    if (system_group) lv_obj_add_flag(system_group, LV_OBJ_FLAG_HIDDEN);

    if (v == 0)      lv_obj_clear_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
    else if (v == 1) lv_obj_clear_flag(idle_group, LV_OBJ_FLAG_HIDDEN);
    else if (current_screen == SCREEN_CODEX && codex_group)
                     lv_obj_clear_flag(codex_group, LV_OBJ_FLAG_HIDDEN);
    else if (current_screen == SCREEN_ANTIGRAVITY && antigravity_group)
                     lv_obj_clear_flag(antigravity_group, LV_OBJ_FLAG_HIDDEN);
    else if (current_screen == SCREEN_SYSTEM && system_group)
                     lv_obj_clear_flag(system_group, LV_OBJ_FLAG_HIDDEN);
    else             lv_obj_clear_flag(usage_group, LV_OBJ_FLAG_HIDDEN);
}

void ui_tick_anim(void) {
    if (current_screen != SCREEN_SYSTEM && current_screen != SCREEN_USAGE && current_screen != SCREEN_CODEX &&
        current_screen != SCREEN_ANTIGRAVITY) return;
    update_view_state();
    if (view_state == 1) splash_mini_tick();   // animate the sleeping creature on the idle screen

    uint32_t now = lv_tick_get();

    // Title clock: once the daemon has sent wall-clock time, replace "Usage" with
    // the live time, advanced locally so it ticks every minute between payloads.
    if (clock_base_epoch > 0 && current_screen != SCREEN_SYSTEM) {
        time_t cur = (time_t)(clock_base_epoch + (now - clock_base_ms) / 1000);
        struct tm tmv;
        gmtime_r(&cur, &tmv);   // epoch is already local wall-clock → gmtime keeps it as-is
        if (tmv.tm_min != clock_last_min) {   // only rewrite the title when the minute changes
            clock_last_min = tmv.tm_min;
            char tbuf[12];
            if (clock_fmt == 12) {
                int h12 = tmv.tm_hour % 12;
                if (h12 == 0) h12 = 12;
                snprintf(tbuf, sizeof(tbuf), "%d:%02d %s", h12, tmv.tm_min,
                         tmv.tm_hour < 12 ? "AM" : "PM");
            } else {
                snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
            }
            lv_label_set_text(lbl_title, tbuf);
        }
    }

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

// Plan line under the title, per tab. Strings come from the daemon; an empty one
// just leaves the line blank rather than inventing a label.
static void apply_subtitle(void) {
    if (!lbl_subtitle) return;
    const char* s = (current_screen == SCREEN_SYSTEM) ? "" :
                    (current_screen == SCREEN_CODEX) ? codex_plan_str :
                    (current_screen == SCREEN_ANTIGRAVITY) ? antigravity_plan_str :
                    claude_plan_str;
    lv_label_set_text(lbl_subtitle, s);
    lv_obj_align(lbl_subtitle, LV_ALIGN_TOP_MID, 16, L.subtitle_y);
    if (current_screen == SCREEN_SPLASH || s[0] == '\0')
        lv_obj_add_flag(lbl_subtitle, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_clear_flag(lbl_subtitle, LV_OBJ_FLAG_HIDDEN);
}

static void apply_battery_visibility(void) {
    if (!battery_img) return;
    if (current_screen == SCREEN_SPLASH) lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    else                                  lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
}

// Splash is now reached ONLY from the logo; tapping the body does nothing.
static void logo_click_cb(lv_event_t* e) {
    (void)e;
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

// Tapping the splash anywhere still returns you to where you were — otherwise
// the logo is hidden there and you'd be stuck.
static void global_click_cb(lv_event_t* e) {
    (void)e;
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
}

// The title toggles the /stats view for the tab you're on.
static void usage_title_cb(lv_event_t* e) {
    (void)e;
    if (current_screen != SCREEN_USAGE && current_screen != SCREEN_CODEX &&
        current_screen != SCREEN_ANTIGRAVITY) return;
    stats_from = current_screen;
    stats_provider = (current_screen == SCREEN_CODEX) ? 1 :
                     (current_screen == SCREEN_ANTIGRAVITY) ? 2 : 0;
    ui_show_screen(SCREEN_STATS);
}

static void stats_title_cb(lv_event_t* e) {
    (void)e;
    ui_show_screen(stats_from);
}

// Horizontal swipe pages between System, Claude, Codex, and Antigravity. LVGL reports a
// gesture on the indev, and it also fires a CLICKED on release — consume the
// gesture so a page swipe doesn't also toggle the splash screen.
static void global_gesture_cb(lv_event_t* e) {
    (void)e;
    lv_indev_t* indev = lv_indev_active();
    if (!indev) return;
    // Touch is rotated into the content frame in my_touch_cb, so LVGL's gesture
    // direction is already the one the user saw — no remap here.
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);

    // Consume EVERY gesture, before any direction test. LVGL fires CLICKED on
    // release regardless, so an unhandled swipe (vertical, or diagonal enough
    // that gesture_sum picks the other axis) used to fall through to
    // global_click_cb and toggle the splash — which is what a sloppy swipe felt
    // like. A swipe must never be read as a tap, whatever direction it lands on.
    lv_indev_wait_release(indev);

    if (current_screen == SCREEN_SPLASH) return;
    if (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT) return;

    // System sits immediately left of Claude; the provider flow remains unchanged.
    if (dir == LV_DIR_LEFT && current_screen == SCREEN_SYSTEM) ui_show_screen(SCREEN_USAGE);
    else if (dir == LV_DIR_RIGHT && current_screen == SCREEN_USAGE) ui_show_screen(SCREEN_SYSTEM);
    else if (dir == LV_DIR_LEFT  && current_screen == SCREEN_USAGE) ui_show_screen(SCREEN_CODEX);
    else if (dir == LV_DIR_LEFT && current_screen == SCREEN_CODEX) ui_show_screen(SCREEN_ANTIGRAVITY);
    else if (dir == LV_DIR_RIGHT && current_screen == SCREEN_ANTIGRAVITY) ui_show_screen(SCREEN_CODEX);
    else if (dir == LV_DIR_RIGHT && current_screen == SCREEN_CODEX) ui_show_screen(SCREEN_USAGE);
}

void ui_show_screen(screen_t screen) {
    lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
    splash_hide();

    if (stats_container) lv_obj_add_flag(stats_container, LV_OBJ_FLAG_HIDDEN);

    switch (screen) {
    case SCREEN_SPLASH:  splash_show(); break;
    case SCREEN_SYSTEM:
    case SCREEN_USAGE:
    case SCREEN_CODEX:
    case SCREEN_ANTIGRAVITY:
        lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_STATS:
        if (stats_container) lv_obj_clear_flag(stats_container, LV_OBJ_FLAG_HIDDEN);
        break;
    default: break;
    }

    if (logo_img) {
        if (screen == SCREEN_SPLASH || screen == SCREEN_SYSTEM) {
            lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
            // Swap the header mark to match the provider you're looking at —
            // including on the stats screen, which belongs to one of the tabs.
            const int provider = (screen == SCREEN_STATS) ? stats_provider :
                                 (screen == SCREEN_CODEX) ? 1 :
                                 (screen == SCREEN_ANTIGRAVITY) ? 2 : 0;
            lv_image_set_src(logo_img, provider == 1 ? &codex_logo_dsc :
                                       provider == 2 ? &gemini_logo_dsc : &logo_dsc);
        }
    }
    if (system_icon) {
        if (screen == SCREEN_SYSTEM) lv_obj_clear_flag(system_icon, LV_OBJ_FLAG_HIDDEN);
        else                         lv_obj_add_flag(system_icon, LV_OBJ_FLAG_HIDDEN);
    }

    if (screen == SCREEN_STATS) render_stats();

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
    if (lbl_title) {
        if (screen == SCREEN_SYSTEM) {
            lv_label_set_text(lbl_title, "System");
            lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, L.title_y);
        } else if (screen != SCREEN_SPLASH) {
            lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 16, L.title_y);
            if (clock_base_epoch == 0) lv_label_set_text(lbl_title, "Usage");
            else clock_last_min = -1;
        }
    }
    view_state = -1;            // force update_view_state() to re-lay-out for this tab
    apply_subtitle();
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
