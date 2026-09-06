#include "ui.h"
#include "splash.h"
#include <lvgl.h>
#include <time.h>
#include "logo.h"
#include "clawd_still.h"
#include "icons.h"
#include "hal/board_caps.h"
#include "history_math.h"
#include "usage_rate.h"

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
    int16_t bar_h;
    int16_t panel_pad_x, panel_pad_y;
    int16_t pill_pad_x, pill_pad_y;
    const lv_font_t* title_font;     // screen title / clock
    const lv_font_t* pct_font;       // big percentage number
    const lv_font_t* ent_pct_font;   // enterprise spending number
    const lv_font_t* pill_font;      // "Current" / "Weekly" pill
    const lv_font_t* reset_font;     // "Resets in ..." line
    const lv_font_t* pace_font;      // enterprise "Under/On/Over pace" line
    const lv_font_t* anim_font;      // animated status line
    int16_t anim_y;                  // status line offset from bottom
    bool    small_icons;             // 40px logo + 24px battery (vs 80/48) on small screens
    int16_t title_nudge;             // title x-shift balancing the corner logo
    int16_t logo_y;                  // logo top edge
    int16_t batt_y;                  // battery icon top edge
    int16_t batt_w;                  // battery icon width, for position math

    // Pairing hint / idle screen
    int16_t pair_y1, pair_y2, pair_y3;
    int16_t idle_px;                 // sleeping-creature size on the idle screen

    // History screen
    int16_t hist_chart_h;            // bar area height inside the chart panel
    int16_t hist_bar_gap;            // px between day bars
    int16_t hist_mix_h;              // segmented model-mix bar height
    const lv_font_t* hist_val_font;  // row values ("1.3M")
    const lv_font_t* hist_lbl_font;  // row labels ("Today")
    const lv_font_t* hist_sub_font;  // weekday initials, sub-lines, legend

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

    // Values shared by the two original breakpoints; the small branch below
    // overrides them wholesale.
    L.bar_h = 24;
    L.panel_pad_x = 16;
    L.panel_pad_y = 12;
    L.pill_pad_x = 18;
    L.pill_pad_y = 6;
    L.title_font   = &font_tiempos_56;
    L.pct_font     = &font_styrene_48;
    L.ent_pct_font = &font_tiempos_56;
    L.pill_font    = &font_styrene_28;
    L.reset_font   = &font_styrene_28;
    L.pace_font    = &font_styrene_16;
    L.anim_font    = &font_mono_32;
    L.anim_y = -15;
    L.small_icons = false;
    L.title_nudge = 16;
    L.logo_y = L.title_y - 10;
    L.batt_y = L.title_y;
    L.batt_w = ICON_BATTERY_W;
    L.pair_y1 = 40;
    L.pair_y2 = 120;
    L.pair_y3 = 160;
    L.idle_px = 160;

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
        L.hist_chart_h  = 100;
        L.hist_bar_gap  = 4;
        L.hist_mix_h    = 14;
        L.hist_val_font = &font_styrene_28;
        L.hist_lbl_font = &font_styrene_20;
        L.hist_sub_font = &font_styrene_16;
    } else if (c.height > 320) {
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
        L.hist_chart_h  = 120;
        L.hist_bar_gap  = 4;
        L.hist_mix_h    = 12;
        L.hist_val_font = &font_styrene_24;
        L.hist_lbl_font = &font_styrene_16;
        L.hist_sub_font = &font_styrene_14;
    } else {
        // Small layout — tuned for 240x240 (LCD-1.54 and similar square TFTs).
        // Everything shrinks: fonts two steps down, panels ~half height, and
        // the corner logo/battery switch to the 40px/24px small assets.
        L.margin = 8;
        L.title_y = 4;
        L.content_y = 44;
        L.usage_panel_h = 74;
        L.usage_panel_gap = 6;
        L.usage_bar_y = 30;
        L.usage_reset_y = 46;
        L.bar_h = 12;
        L.panel_pad_x = 10;
        L.panel_pad_y = 6;
        L.pill_pad_x = 8;
        L.pill_pad_y = 2;
        L.title_font   = &font_tiempos_34;
        L.pct_font     = &font_styrene_24;
        L.ent_pct_font = &font_tiempos_34;
        L.pill_font    = &font_styrene_14;
        L.reset_font   = &font_styrene_14;
        L.pace_font    = &font_styrene_12;
        L.anim_font    = &font_mono_18;
        // Center the status line in the strip below the weekly panel; flush
        // against the bottom edge it reads as unevenly spaced.
        L.anim_y = -10;
        L.small_icons = true;
        L.title_nudge = 8;
        L.logo_y = 2;
        L.batt_y = 10;
        L.batt_w = ICON_BATTERY_SMALL_W;
        L.pair_y1 = 12;
        L.pair_y2 = 56;
        L.pair_y3 = 80;
        L.idle_px = 96;
        L.bt_info_panel_h = 90;
        L.bt_reset_zone_h = 60;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_20;
        L.bt_device_font   = &font_styrene_14;
        L.bt_credit_1_font = &font_styrene_12;
        L.bt_credit_2_font = &font_styrene_12;
        L.hist_chart_h  = 84;
        L.hist_bar_gap  = 2;
        L.hist_mix_h    = 8;
        L.hist_val_font = &font_styrene_16;
        L.hist_lbl_font = &font_styrene_14;
        L.hist_sub_font = &font_styrene_12;
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
// Enterprise-only widgets inside panel_session
static lv_obj_t* lbl_session_pct_sym = nullptr;  // "%" in smaller font
static lv_obj_t* lbl_spending_desc = nullptr;     // "of your monthly budget"
static lv_obj_t* lbl_spending_status = nullptr;   // "Under pace" / "On pace" / "Over pace"
static lv_obj_t* lbl_anim;      // status line: connection state + whimsical idle

// ---- History screen widgets ----
// A 14-day bar chart of output tokens with the current week's running total
// drawn over it, then Today / This week / Pace rows and a model-mix bar.
// Bars and the line are plain objects (not lv_chart): per-day colour and an
// overlaid line on its own scale are simpler to control that way, and it
// stays within the widget set every env already compiles.
static lv_obj_t* history_container;
static lv_obj_t* lbl_hist_title;
static lv_obj_t* hist_body;                    // everything but the title; hidden until data lands
static lv_obj_t* lbl_hist_empty;               // "No history yet"
static lv_obj_t* hist_panel;
static lv_obj_t* hist_bars[HIST_DAYS];
// Same shared-style discipline as the maxing cells: 14 bars with local styles
// cost several KB of LVGL's 64 KB pool for no benefit.
static lv_style_t hist_bar_style[3];   // 0 = empty day, 1 = used, 2 = today
static lv_obj_t* hist_day_lbls[HIST_DAYS];
static lv_obj_t* hist_line;
static lv_point_precise_t hist_line_pts[HIST_DAYS];
static lv_obj_t* lbl_hist_today_v;
static lv_obj_t* lbl_hist_today_s;
static lv_obj_t* lbl_hist_week_v;
static lv_obj_t* lbl_hist_week_s;
static lv_obj_t* lbl_hist_pace_v;
static lv_obj_t* lbl_hist_pace_s;
static lv_obj_t* hist_mix_segs[HIST_MIX_N];
static lv_obj_t* lbl_hist_mix;
static bool      hist_has_data = false;
static float     hist_session_pct = 0.0f;      // live numbers the pace row re-projects from
static int       hist_reset_mins = -1;
static bool      hist_live = false;            // last payload was ok (pace is meaningful)
static uint32_t  hist_pace_ms = 0;             // last pace refresh (rate warms up over time)
#define HIST_PACE_REFRESH_MS 10000

// ---- Battery indicator (shared, on top) ----
// ---- Maxing screen (5h window grid) ----
// One row per local day, one cell per 5-hour window in the order they opened,
// shaded by how much of the limit that window used. Answers "when did I max
// out, and how often" at a glance.
static lv_obj_t* maxing_container;
static lv_obj_t* lbl_max_title;
static lv_obj_t* lbl_max_head;
static lv_obj_t* lbl_max_foot;
static lv_obj_t* maxing_body;
static lv_obj_t* max_cells[HIST_GRID_DAYS][HIST_WIN_PER_DAY];
// Cell appearance lives in shared styles, not per-object local styles: a local
// style allocates its own property array on every object, and 35 cells of that
// blew LVGL's 64 KB pool (splash_init then failed to allocate). Shared styles
// are one allocation total, swapped by pointer on update.
static lv_style_t max_style_level[4];
static lv_style_t max_style_empty;      // band with no window — a faint track
// Borders mark cells worth a second look. A plain cell is settled fact: a
// window whose peak the daemon actually measured. Grey = the level is inferred
// from token volume; white = the window burning right now. As more windows get
// measured, the grey borders fade out on their own.
static lv_style_t max_style_estimated;
static lv_style_t max_style_current;
static lv_obj_t*  max_bandlbl[HIST_BANDS];
static lv_obj_t* max_daylbl[HIST_GRID_DAYS];

static lv_obj_t* battery_img;
static lv_obj_t* logo_img;
static lv_image_dsc_t battery_dscs[5];  // empty, low, medium, full, charging

// ---- Live-data freshness → which usage sub-view to show ----
// usage panels when data is flowing, an idle "Zzz" screen when the host is
// connected but no usage update landed within DATA_FRESH_MS, the pairing hint
// when BLE is down. Re-evaluated every loop in ui_tick_anim().
static lv_obj_t* idle_group;            // the "Zzz" idle screen
static uint32_t  last_data_ms = 0;      // lv_tick when the last valid usage update landed
static bool      data_received = false; // any valid update since boot
static bool      data_ok = true;        // last payload's ok flag; a {"ok":false} beat = "no fresh data"
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
    lv_obj_set_style_pad_left(panel, L.panel_pad_x, 0);
    lv_obj_set_style_pad_right(panel, L.panel_pad_x, 0);
    lv_obj_set_style_pad_top(panel, L.panel_pad_y, 0);
    lv_obj_set_style_pad_bottom(panel, L.panel_pad_y, 0);
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
    lv_obj_set_style_pad_left(lbl, L.pill_pad_x, 0);
    lv_obj_set_style_pad_right(lbl, L.pill_pad_x, 0);
    lv_obj_set_style_pad_top(lbl, L.pill_pad_y, 0);
    lv_obj_set_style_pad_bottom(lbl, L.pill_pad_y, 0);
    return lbl;
}

static void init_battery_icons(void) {
    if (L.small_icons) {
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

static lv_obj_t* make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                                  lv_obj_t** out_pct, lv_obj_t** out_pill,
                                  lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, L.margin, y, L.content_w, L.usage_panel_h);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, L.pct_font, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, L.usage_bar_y,
                        L.content_w - 2 * L.panel_pad_x, L.bar_h);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, L.reset_font, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, L.usage_reset_y);

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
    lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, L.pair_y1);

    lv_obj_t* l2 = lv_label_create(pair_group);
    lv_label_set_text(l2, "hold the power button");
    lv_obj_set_style_text_font(l2, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l2, COL_DIM, 0);
    lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, L.pair_y2);

    lv_obj_t* l3 = lv_label_create(pair_group);
    lv_label_set_text(l3, "for 3 seconds, then release");
    lv_obj_set_style_text_font(l3, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l3, COL_DIM, 0);
    lv_obj_align(l3, LV_ALIGN_TOP_MID, 0, L.pair_y3);

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

    // A shrunk-down resting creature (the official cloud-ride animation)
    // sits between the header and the status line; the animated "Listening…"
    // status line carries the words, so no extra text is needed here.
    lv_obj_t* creature = splash_mini_create(idle_group, "cloud", L.idle_px);
    if (creature) lv_obj_align(creature, LV_ALIGN_CENTER, 0, -20);

    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);  // update_view_state decides
}

// One "label left, value right, sub-line under the value" stat row.
static int hist_row_pitch(void) {
    return lv_font_get_line_height(L.hist_val_font)
         + lv_font_get_line_height(L.hist_sub_font)
         + (L.small_icons ? 4 : 2);
}

static void make_hist_row(lv_obj_t* parent, int y, const char* label,
                          lv_obj_t** value, lv_obj_t** sub) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, L.hist_lbl_font, 0);
    lv_obj_set_style_text_color(l, COL_DIM, 0);
    lv_obj_set_pos(l, L.margin, y + lv_font_get_line_height(L.hist_val_font)
                                  - lv_font_get_line_height(L.hist_lbl_font));

    *value = lv_label_create(parent);
    lv_label_set_text(*value, "--");
    lv_obj_set_style_text_font(*value, L.hist_val_font, 0);
    lv_obj_set_style_text_color(*value, COL_TEXT, 0);
    lv_obj_align(*value, LV_ALIGN_TOP_RIGHT, -L.margin, y);

    *sub = lv_label_create(parent);
    lv_label_set_text(*sub, "");
    lv_label_set_recolor(*sub, true);
    lv_obj_set_style_text_font(*sub, L.hist_sub_font, 0);
    lv_obj_set_style_text_color(*sub, COL_DIM, 0);
    lv_obj_align(*sub, LV_ALIGN_TOP_RIGHT, -L.margin,
                 y + lv_font_get_line_height(L.hist_val_font) + 1);
}

static void init_history_screen(lv_obj_t* scr) {
    history_container = lv_obj_create(scr);
    lv_obj_set_size(history_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(history_container, 0, 0);
    lv_obj_set_style_bg_opa(history_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(history_container, 0, 0);
    lv_obj_set_style_pad_all(history_container, 0, 0);
    lv_obj_clear_flag(history_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(history_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lbl_hist_title = lv_label_create(history_container);
    lv_label_set_text(lbl_hist_title, "History");
    lv_obj_set_style_text_font(lbl_hist_title, L.title_font, 0);
    lv_obj_set_style_text_color(lbl_hist_title, COL_TEXT, 0);
    lv_obj_align(lbl_hist_title, LV_ALIGN_TOP_MID, L.title_nudge, L.title_y);

    lbl_hist_empty = lv_label_create(history_container);
    lv_label_set_text(lbl_hist_empty, "No history yet");
    lv_obj_set_style_text_font(lbl_hist_empty, L.hist_lbl_font, 0);
    lv_obj_set_style_text_color(lbl_hist_empty, COL_DIM, 0);
    lv_obj_align(lbl_hist_empty, LV_ALIGN_CENTER, 0, 0);

    hist_body = lv_obj_create(history_container);
    lv_obj_set_size(hist_body, L.scr_w, L.scr_h);
    lv_obj_set_pos(hist_body, 0, 0);
    lv_obj_set_style_bg_opa(hist_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hist_body, 0, 0);
    lv_obj_set_style_pad_all(hist_body, 0, 0);
    lv_obj_clear_flag(hist_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(hist_body, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(hist_body, LV_OBJ_FLAG_HIDDEN);

    // ---- chart panel: 14 bars + weekday initials + the week's running total
    const int day_lbl_h = lv_font_get_line_height(L.hist_sub_font);
    const int panel_h = 2 * L.panel_pad_y + L.hist_chart_h + 2 + day_lbl_h;
    hist_panel = make_panel(hist_body, L.margin, L.content_y, L.content_w, panel_h);

    for (int i = 0; i < 3; ++i) {
        lv_style_init(&hist_bar_style[i]);
        lv_style_set_bg_color(&hist_bar_style[i], i == 2 ? COL_ACCENT : i == 1 ? COL_DIM : COL_BAR_BG);
        lv_style_set_bg_opa(&hist_bar_style[i], LV_OPA_COVER);
        lv_style_set_radius(&hist_bar_style[i], 2);
        lv_style_set_border_width(&hist_bar_style[i], 0);
        lv_style_set_pad_all(&hist_bar_style[i], 0);
    }

    const int inner_w = L.content_w - 2 * L.panel_pad_x;
    const int bar_w = (inner_w - (HIST_DAYS - 1) * L.hist_bar_gap) / HIST_DAYS;
    const int used_w = HIST_DAYS * bar_w + (HIST_DAYS - 1) * L.hist_bar_gap;
    const int x0 = (inner_w - used_w) / 2;   // centre the leftover pixels
    for (int i = 0; i < HIST_DAYS; ++i) {
        const int x = x0 + i * (bar_w + L.hist_bar_gap);
        hist_bars[i] = lv_obj_create(hist_panel);
        lv_obj_remove_style_all(hist_bars[i]);
        lv_obj_add_style(hist_bars[i], &hist_bar_style[0], 0);
        lv_obj_set_size(hist_bars[i], bar_w, 2);
        lv_obj_set_pos(hist_bars[i], x, L.hist_chart_h - 2);
        lv_obj_clear_flag(hist_bars[i], (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

        hist_day_lbls[i] = lv_label_create(hist_panel);
        lv_label_set_text(hist_day_lbls[i], "");
        lv_obj_set_style_text_font(hist_day_lbls[i], L.hist_sub_font, 0);
        lv_obj_set_style_text_color(hist_day_lbls[i], COL_DIM, 0);
        lv_obj_set_style_text_align(hist_day_lbls[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(hist_day_lbls[i], bar_w + L.hist_bar_gap);
        lv_obj_set_pos(hist_day_lbls[i], x - L.hist_bar_gap / 2, L.hist_chart_h + 2);

        hist_line_pts[i].x = x + bar_w / 2;
        hist_line_pts[i].y = L.hist_chart_h;
    }
    hist_line = lv_line_create(hist_panel);
    lv_obj_set_pos(hist_line, 0, 0);
    lv_obj_set_style_line_width(hist_line, L.small_icons ? 2 : 3, 0);
    lv_obj_set_style_line_color(hist_line, COL_TEXT, 0);
    lv_obj_set_style_line_rounded(hist_line, true, 0);
    lv_obj_clear_flag(hist_line, LV_OBJ_FLAG_CLICKABLE);

    // ---- stat rows
    const int pitch = hist_row_pitch();
    int y = L.content_y + panel_h + L.usage_panel_gap;
    make_hist_row(hist_body, y, "Today",     &lbl_hist_today_v, &lbl_hist_today_s); y += pitch;
    make_hist_row(hist_body, y, "This week", &lbl_hist_week_v,  &lbl_hist_week_s);  y += pitch;
    make_hist_row(hist_body, y, "Pace",      &lbl_hist_pace_v,  &lbl_hist_pace_s);  y += pitch;

    // ---- model mix: a segmented bar over a track, legend underneath
    lv_obj_t* track = lv_obj_create(hist_body);
    lv_obj_set_size(track, L.content_w, L.hist_mix_h);
    lv_obj_set_pos(track, L.margin, y + 2);
    lv_obj_set_style_bg_color(track, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, L.hist_mix_h / 2, 0);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_set_style_pad_all(track, 0, 0);
    lv_obj_clear_flag(track, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    for (int i = 0; i < HIST_MIX_N; ++i) {
        hist_mix_segs[i] = lv_obj_create(track);
        lv_obj_set_size(hist_mix_segs[i], 0, L.hist_mix_h);
        lv_obj_set_pos(hist_mix_segs[i], 0, 0);
        lv_obj_set_style_bg_opa(hist_mix_segs[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(hist_mix_segs[i], L.hist_mix_h / 2, 0);
        lv_obj_set_style_border_width(hist_mix_segs[i], 0, 0);
        lv_obj_set_style_pad_all(hist_mix_segs[i], 0, 0);
        lv_obj_clear_flag(hist_mix_segs[i], (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        lv_obj_add_flag(hist_mix_segs[i], LV_OBJ_FLAG_HIDDEN);
    }
    lbl_hist_mix = lv_label_create(hist_body);
    lv_label_set_text(lbl_hist_mix, "");
    lv_label_set_recolor(lbl_hist_mix, true);
    lv_obj_set_style_text_font(lbl_hist_mix, L.hist_sub_font, 0);
    lv_obj_set_style_text_color(lbl_hist_mix, COL_DIM, 0);
    lv_obj_set_pos(lbl_hist_mix, L.margin, y + 2 + L.hist_mix_h + 4);

    lv_obj_add_flag(history_container, LV_OBJ_FLAG_HIDDEN);   // ui_show_screen decides
}

static const lv_color_t hist_mix_colors[HIST_MIX_N] = {
    lv_color_hex(0xd97757),   // COL_ACCENT
    lv_color_hex(0x788c5d),   // COL_GREEN
    lv_color_hex(0xb0aea5),   // COL_DIM
};
static const char* const hist_mix_hex[HIST_MIX_N] = { "d97757", "788c5d", "b0aea5" };

// Re-project the pace row from the latest live numbers and the smoothed rate.
// Called on every payload and every HIST_PACE_REFRESH_MS while the screen is
// up, because the rate tracker keeps warming up between payloads.
static void update_history_pace(void) {
    if (!lbl_hist_pace_v) return;
    char v[32], sub[48];
    lv_color_t col = COL_DIM;
    float rate = hist_live ? usage_rate_pct_per_min() : -1.0f;
    PaceProjection p = history_pace(hist_session_pct, hist_reset_mins, rate);
    switch (p.kind) {
    case PACE_LIMIT:
        if (p.minutes < 60) snprintf(v, sizeof v, "Limit in %dm", p.minutes);
        else                snprintf(v, sizeof v, "Limit in %dh %dm", p.minutes / 60, p.minutes % 60);
        col = (p.minutes < 30) ? COL_RED : COL_AMBER;
        snprintf(sub, sizeof sub, "%.2f%%/min, before reset", (double)rate);
        break;
    case PACE_OK:
        snprintf(v, sizeof v, "%d%% at reset", p.pct);
        col = COL_GREEN;
        snprintf(sub, sizeof sub, "%.2f%%/min", (double)rate);
        break;
    case PACE_IDLE:
        snprintf(v, sizeof v, "Idle");
        snprintf(sub, sizeof sub, "no burn in the last few min");
        break;
    default:
        snprintf(v, sizeof v, hist_live ? "Warming up" : "--");
        snprintf(sub, sizeof sub, hist_live ? "needs a few minutes of samples" : "no live session data");
        break;
    }
    lv_label_set_text(lbl_hist_pace_v, v);
    lv_obj_set_style_text_color(lbl_hist_pace_v, col, 0);
    lv_label_set_text(lbl_hist_pace_s, sub);
    hist_pace_ms = lv_tick_get();
}

static void update_history(const UsageData* d) {
    if (!history_container) return;
    // Live numbers for the pace row travel on every payload, history only on
    // ones that carry it — an old daemon's beat must not blank the chart.
    hist_live = d->ok;
    if (d->ok) {
        hist_session_pct = d->session_pct;
        hist_reset_mins  = d->session_reset_mins;
    }
    if (d->hist_days <= 0) {
        update_history_pace();
        return;
    }

    hist_has_data = true;
    lv_obj_add_flag(lbl_hist_empty, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(hist_body, LV_OBJ_FLAG_HIDDEN);

    const int16_t* out = d->hist_out_k;
    const int n = HIST_DAYS;                    // parse right-aligns short arrays
    const int32_t vmax = history_max(out, n);
    static const char wd[7] = { 'M', 'T', 'W', 'T', 'F', 'S', 'S' };
    for (int i = 0; i < n; ++i) {
        int h = (int)((int32_t)out[i] * L.hist_chart_h / vmax);
        if (h < 2) h = 2;
        lv_obj_set_height(hist_bars[i], h);
        lv_obj_set_y(hist_bars[i], L.hist_chart_h - h);
        const bool today = (i == n - 1);
        // Remove the specific shared styles only — lv_obj_remove_style(NULL)
        // would also drop the object's *local* style, which is where
        // lv_obj_set_size/set_pos live.
        for (int k = 0; k < 3; ++k) lv_obj_remove_style(hist_bars[i], &hist_bar_style[k], 0);
        lv_obj_add_style(hist_bars[i], &hist_bar_style[today ? 2 : (out[i] > 0 ? 1 : 0)], 0);
        int w = ((d->hist_weekday - (n - 1 - i)) % 7 + 7) % 7;
        char t[2] = { wd[w], 0 };
        lv_label_set_text(hist_day_lbls[i], t);
        lv_obj_set_style_text_color(hist_day_lbls[i], today ? COL_ACCENT : COL_DIM, 0);
    }

    // Running total for "this week" — the rolling 7-day limit window when the
    // daemon told us where it opened, else the calendar week — on its own
    // scale so it always climbs to the top-right corner.
    int32_t cum[HIST_DAYS];
    const int start = history_week_start(n, d->hist_weekday, d->hist_week_start);
    const int week_len = history_week_cumulative(out, n, start, cum);
    const int32_t cmax = (cum[n - 1] > 0) ? cum[n - 1] : 1;
    for (int i = n - week_len; i < n; ++i) {
        hist_line_pts[i].y = L.hist_chart_h - (int)(cum[i] * (int32_t)L.hist_chart_h / cmax);
    }
    lv_line_set_points(hist_line, &hist_line_pts[n - week_len], week_len);

    char a[8], b[8], buf[64];
    int32_t today_k, avg_k;
    history_today_vs_avg(out, n, &today_k, &avg_k);
    history_fmt_k(today_k, a, sizeof a);
    lv_label_set_text(lbl_hist_today_v, a);
    if (avg_k > 0) {
        const int delta = (int)((today_k - avg_k) * 100 / avg_k);
        snprintf(buf, sizeof buf, "%d turns, %+d%% vs 7d avg", d->hist_turns[n - 1], delta);
    } else {
        snprintf(buf, sizeof buf, "%d turns", d->hist_turns[n - 1]);
    }
    lv_label_set_text(lbl_hist_today_s, buf);

    int32_t week_turns = 0;
    for (int i = start; i < n; ++i) week_turns += d->hist_turns[i];
    history_fmt_k(cum[n - 1], b, sizeof b);
    lv_label_set_text(lbl_hist_week_v, b);
    static const char* const wd_name[7] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
    const int start_wd = ((d->hist_weekday - (n - 1 - start)) % 7 + 7) % 7;
    if (d->hist_week_start >= 0 && d->ok && d->weekly_reset_mins > 0) {
        // Rolling window: say when it opened and when the meter resets.
        const int m = d->weekly_reset_mins;
        if (m >= 1440) snprintf(buf, sizeof buf, "%ld turns since %s, resets %dd %dh",
                                (long)week_turns, wd_name[start_wd], m / 1440, (m % 1440) / 60);
        else           snprintf(buf, sizeof buf, "%ld turns since %s, resets %dh %dm",
                                (long)week_turns, wd_name[start_wd], m / 60, m % 60);
    } else {
        snprintf(buf, sizeof buf, "%ld turns since %s", (long)week_turns, wd_name[start_wd]);
    }
    lv_label_set_text(lbl_hist_week_s, buf);

    update_history_pace();

    // Model mix: segments sized by share, legend recoloured to match.
    int x = 0;
    char legend[96] = "";
    size_t len = 0;
    for (int i = 0; i < HIST_MIX_N; ++i) {
        if (i >= d->hist_mix_n) { lv_obj_add_flag(hist_mix_segs[i], LV_OBJ_FLAG_HIDDEN); continue; }
        int w = (int)((int32_t)d->hist_mix_pct[i] * L.content_w / 100);
        if (x + w > L.content_w) w = L.content_w - x;
        lv_obj_clear_flag(hist_mix_segs[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(hist_mix_segs[i], hist_mix_colors[i], 0);
        lv_obj_set_size(hist_mix_segs[i], w, L.hist_mix_h);
        lv_obj_set_x(hist_mix_segs[i], x);
        x += w;
        len += snprintf(legend + len, sizeof legend - len, "%s#%s %s# %d%%",
                        i ? "  " : "", hist_mix_hex[i], d->hist_mix_name[i], d->hist_mix_pct[i]);
        if (len >= sizeof legend) break;
    }
    lv_label_set_text(lbl_hist_mix, legend);
}

// Level palette: barely-used → maxed. Deliberately the usage screen's ramp so
// a hot window reads the same here as a hot bar does there.
static lv_color_t maxing_level_color(int level) {
    switch (level) {
    case 3:  return COL_RED;
    case 2:  return COL_ACCENT;
    case 1:  return COL_GREEN;
    // A window that barely moved: dim, but unmistakably a window. An empty
    // band uses COL_PANEL below, which is darker still.
    default: return lv_color_hex(0x4a5240);
    }
}

static void init_maxing_styles(void) {
    for (int i = 0; i < 4; ++i) {
        lv_style_init(&max_style_level[i]);
        lv_style_set_bg_color(&max_style_level[i], maxing_level_color(i));
        lv_style_set_bg_opa(&max_style_level[i], LV_OPA_COVER);
        lv_style_set_radius(&max_style_level[i], 3);
        lv_style_set_border_width(&max_style_level[i], 0);
        lv_style_set_pad_all(&max_style_level[i], 0);
    }
    lv_style_init(&max_style_empty);
    lv_style_set_bg_color(&max_style_empty, COL_PANEL);
    lv_style_set_bg_opa(&max_style_empty, LV_OPA_COVER);
    lv_style_set_radius(&max_style_empty, 3);
    lv_style_set_border_width(&max_style_empty, 0);
    lv_style_set_pad_all(&max_style_empty, 0);

    lv_style_init(&max_style_estimated);
    lv_style_set_border_width(&max_style_estimated, 1);
    lv_style_set_border_color(&max_style_estimated, COL_DIM);
    lv_style_set_border_opa(&max_style_estimated, LV_OPA_60);

    lv_style_init(&max_style_current);
    lv_style_set_border_width(&max_style_current, 2);
    lv_style_set_border_color(&max_style_current, COL_TEXT);
    lv_style_set_border_opa(&max_style_current, LV_OPA_COVER);
}

static void init_maxing_screen(lv_obj_t* scr) {
    maxing_container = lv_obj_create(scr);
    lv_obj_set_size(maxing_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(maxing_container, 0, 0);
    lv_obj_set_style_bg_opa(maxing_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(maxing_container, 0, 0);
    lv_obj_set_style_pad_all(maxing_container, 0, 0);
    lv_obj_clear_flag(maxing_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(maxing_container, global_click_cb, LV_EVENT_CLICKED, NULL);
    init_maxing_styles();

    lbl_max_title = lv_label_create(maxing_container);
    lv_label_set_text(lbl_max_title, "Maxing");
    lv_obj_set_style_text_font(lbl_max_title, L.title_font, 0);
    lv_obj_set_style_text_color(lbl_max_title, COL_TEXT, 0);
    lv_obj_align(lbl_max_title, LV_ALIGN_TOP_MID, L.title_nudge, L.title_y);

    maxing_body = lv_obj_create(maxing_container);
    lv_obj_set_size(maxing_body, L.scr_w, L.scr_h);
    lv_obj_set_pos(maxing_body, 0, 0);
    lv_obj_set_style_bg_opa(maxing_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(maxing_body, 0, 0);
    lv_obj_set_style_pad_all(maxing_body, 0, 0);
    lv_obj_clear_flag(maxing_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(maxing_body, LV_OBJ_FLAG_EVENT_BUBBLE);

    lbl_max_head = lv_label_create(maxing_body);
    lv_label_set_recolor(lbl_max_head, true);
    lv_label_set_text(lbl_max_head, "");
    lv_obj_set_style_text_font(lbl_max_head, L.hist_lbl_font, 0);
    lv_obj_set_style_text_color(lbl_max_head, COL_DIM, 0);

    lbl_max_foot = lv_label_create(maxing_body);
    lv_label_set_text(lbl_max_foot, "");
    lv_obj_set_style_text_font(lbl_max_foot, L.hist_sub_font, 0);
    lv_obj_set_style_text_color(lbl_max_foot, COL_DIM, 0);
    lv_obj_align(lbl_max_foot, LV_ALIGN_BOTTOM_LEFT, L.margin, -L.margin / 2);

    const int lbl_w = lv_font_get_line_height(L.hist_sub_font) * 2;

    lv_obj_set_pos(lbl_max_head, L.margin, L.content_y);

    // Grid geometry: one row per day, one cell per window.
    const int grid_y = L.content_y + lv_font_get_line_height(L.hist_lbl_font) + 8;
    const int band_h = lv_font_get_line_height(L.hist_sub_font) + 2;
    const int rows_y  = grid_y + band_h;
    const int foot_h = lv_font_get_line_height(L.hist_sub_font) + L.margin;
    const int avail  = L.scr_h - rows_y - foot_h;
    const int row_h  = avail / HIST_GRID_DAYS;
    const int cell_h = row_h - (L.small_icons ? 3 : 5);
    const int cells_w = L.content_w - lbl_w - 4;
    const int gap    = L.small_icons ? 2 : 4;
    const int cell_w = (cells_w - (HIST_WIN_PER_DAY - 1) * gap) / HIST_WIN_PER_DAY;

    // Column header: the hour each band starts, so the grid reads as a clock.
    static const char* const band_lbl[HIST_BANDS] = { "00", "05", "10", "15", "20" };
    for (int b = 0; b < HIST_BANDS; ++b) {
        max_bandlbl[b] = lv_label_create(maxing_body);
        lv_label_set_text(max_bandlbl[b], band_lbl[b]);
        lv_obj_set_style_text_font(max_bandlbl[b], L.hist_sub_font, 0);
        lv_obj_set_style_text_color(max_bandlbl[b], COL_DIM, 0);
        lv_obj_set_width(max_bandlbl[b], cell_w);
        lv_obj_set_style_text_align(max_bandlbl[b], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(max_bandlbl[b], L.margin + lbl_w + 4 + b * (cell_w + gap), grid_y);
    }

    static const char* const wd_short[7] = { "M", "T", "W", "T", "F", "S", "S" };
    for (int d = 0; d < HIST_GRID_DAYS; ++d) {
        const int y = rows_y + d * row_h;
        max_daylbl[d] = lv_label_create(maxing_body);
        lv_label_set_text(max_daylbl[d], wd_short[d]);
        lv_obj_set_style_text_font(max_daylbl[d], L.hist_sub_font, 0);
        lv_obj_set_style_text_color(max_daylbl[d], COL_DIM, 0);
        lv_obj_set_width(max_daylbl[d], lbl_w);
        lv_obj_set_style_text_align(max_daylbl[d], LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_pos(max_daylbl[d], L.margin, y + (cell_h - lv_font_get_line_height(L.hist_sub_font)) / 2);

        for (int w = 0; w < HIST_WIN_PER_DAY; ++w) {
            lv_obj_t* cell = lv_obj_create(maxing_body);
            max_cells[d][w] = cell;
            lv_obj_remove_style_all(cell);          // drop the theme's own local styles
            lv_obj_add_style(cell, &max_style_level[0], 0);
            lv_obj_set_size(cell, cell_w, cell_h);
            lv_obj_set_pos(cell, L.margin + lbl_w + 4 + w * (cell_w + gap), y);
            lv_obj_clear_flag(cell, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
            lv_obj_add_flag(cell, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_add_flag(maxing_container, LV_OBJ_FLAG_HIDDEN);
}

static void update_maxing(const UsageData* d) {
    if (!maxing_container || d->win_days <= 0) return;

    char buf[80];
    int measured = 0;
    for (int i = 0; i < d->win_days; ++i)
        for (const char* c = d->win_grid[i]; *c; ++c)
            if (window_measured(*c)) measured++;

    snprintf(buf, sizeof buf, "#c0392b %d maxed# of %d windows", d->win_maxed, d->win_count);
    lv_label_set_text(lbl_max_head, buf);

    static const char* const wd_short[7] = { "M", "T", "W", "T", "F", "S", "S" };
    for (int row = 0; row < HIST_GRID_DAYS; ++row) {
        // Row 0 is the oldest day in the grid; label it with its real weekday.
        const int wd = ((d->hist_weekday - (HIST_GRID_DAYS - 1 - row)) % 7 + 7) % 7;
        lv_label_set_text(max_daylbl[row], wd_short[wd]);
        const bool today = (row == HIST_GRID_DAYS - 1);
        lv_obj_set_style_text_color(max_daylbl[row], today ? COL_ACCENT : COL_DIM, 0);

        const char* src = (row < d->win_days) ? d->win_grid[row] : "";
        for (int w = 0; w < HIST_WIN_PER_DAY; ++w) {
            const char c = src[w] ? src[w] : 0;
            const int level = c ? window_level(c) : -1;
            lv_obj_t* cell = max_cells[row][w];
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_HIDDEN);
            // Specific styles only — see the note on the history bars.
            for (int k = 0; k < 4; ++k) lv_obj_remove_style(cell, &max_style_level[k], 0);
            lv_obj_remove_style(cell, &max_style_empty, 0);
            lv_obj_remove_style(cell, &max_style_estimated, 0);
            lv_obj_remove_style(cell, &max_style_current, 0);
            const bool is_current = (d->win_current >= 0)
                                 && (d->win_current == row * HIST_BANDS + w);
            if (level < 0) {
                // No window opened in this band — a faint track keeps the
                // column structure readable.
                lv_obj_add_style(cell, &max_style_empty, 0);
                continue;
            }
            lv_obj_add_style(cell, &max_style_level[level], 0);
            // Current wins over provenance: there is only ever one live
            // window, and knowing which one you're burning beats knowing
            // how its level was arrived at.
            if (is_current)                 lv_obj_add_style(cell, &max_style_current, 0);
            else if (!window_measured(c))   lv_obj_add_style(cell, &max_style_estimated, 0);
        }
    }

    const int estimated = d->win_count - measured;
    if (estimated <= 0)            snprintf(buf, sizeof buf, "all measured");
    else if (d->win_current >= 0)  snprintf(buf, sizeof buf, "white = now, grey = est (%d)", estimated);
    else                           snprintf(buf, sizeof buf, "grey border = estimated (%d)", estimated);
    lv_label_set_text(lbl_max_foot, buf);
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
    lv_obj_set_style_text_font(lbl_title, L.title_font, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    // The nudge balances the corner logo on the left; smaller on small
    // screens where the logo is 40px and the battery icon sits closer.
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, L.title_nudge, L.title_y);

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
    lv_obj_set_style_text_font(lbl_session_pct_sym, L.reset_font, 0);
    lv_obj_set_style_text_color(lbl_session_pct_sym, COL_TEXT, 0);
    lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_desc = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_desc, "of your monthly budget");
    lv_obj_set_style_text_font(lbl_spending_desc, L.reset_font, 0);
    lv_obj_set_style_text_color(lbl_spending_desc, COL_DIM, 0);
    lv_obj_set_pos(lbl_spending_desc, 0, L.usage_reset_y);
    lv_obj_add_flag(lbl_spending_desc, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_status = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_status, "");
    lv_obj_set_style_text_font(lbl_spending_status, L.pace_font, 0);
    lv_obj_set_pos(lbl_spending_status, 0, L.usage_reset_y + 20);
    lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);

    panel_weekly = make_usage_panel(usage_group,
                     L.content_y + L.usage_panel_h + L.usage_panel_gap, "Weekly",
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset);
    // Recolor enabled so enterprise period box can color pace and reset separately
    lv_label_set_recolor(lbl_weekly_reset, true);

    build_pair_group(usage_container);
    build_idle_group(usage_container);

    // Status line — always visible on the usage view. Driven by ui_tick_anim().
    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, L.anim_font, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, L.anim_y);
}

// ======== Public API ========

void ui_init(void) {
    compute_layout(board_caps());

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

#ifndef BOARD_HAS_PSRAM
    // Static corner mascot (see clawd_still.h) — the animated one needs PSRAM.
    if (L.small_icons) init_icon_dsc_rgb565a8(&logo_dsc, CLAWD_STILL_SMALL_W, CLAWD_STILL_SMALL_H, clawd_still_small_data);
    else               init_icon_dsc_rgb565a8(&logo_dsc, CLAWD_STILL_W, CLAWD_STILL_H, clawd_still_data);
#endif
    init_battery_icons();

    init_usage_screen(scr);
    init_history_screen(scr);
    init_maxing_screen(scr);
    splash_init(scr);

    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    // Corner mascot in the old logo slot. The still Clawd is shorter than the
    // 80/40 px slot the spark logo used; center it vertically in that slot.
    {
        const int slot  = L.small_icons ? LOGO_SMALL_HEIGHT : LOGO_HEIGHT;
        const int art_h = L.small_icons ? CLAWD_STILL_SMALL_H : CLAWD_STILL_H;
        const int top   = L.logo_y + (slot - art_h) / 2;
#ifdef BOARD_HAS_PSRAM
        // Animated: idles, does acts, and takes walk-off/lurk trips.
        splash_mascot_create(scr, L.margin, top + art_h, L.small_icons ? 2 : 3);
#else
        logo_img = lv_image_create(scr);
        lv_image_set_src(logo_img, &logo_dsc);
        lv_obj_set_pos(logo_img, L.margin, top);
#endif
    }

    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    lv_obj_set_pos(battery_img, L.scr_w - L.batt_w - L.margin, L.batt_y);
    // Boards without battery telemetry never show the indicator (per the HAL
    // contract; previously every board drew the empty-battery glyph).
    if (!board_caps().has_battery) {
        lv_obj_del(battery_img);
        battery_img = nullptr;
    }
}

void ui_update(const UsageData* data) {
    if (!data->valid) return;
    update_history(data);           // local history needs no token; refresh on any beat
    update_maxing(data);
    data_ok = data->ok;
    if (!data->ok) return;          // a {"ok":false} "no data" beat → fall through to idle, keep last numbers
    last_data_ms = lv_tick_get();   // a real usage update just landed
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

    if (data->enterprise) {
        // Spending box: big number-only label + small "%" symbol + desc + pace
        lv_obj_set_style_text_font(lbl_session_pct, L.ent_pct_font, 0);
        lv_label_set_text(lbl_session_label, "Spending");
        lv_obj_add_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status,   LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_style_text_font(lbl_session_pct, L.pct_font, 0);
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
    } else if (data_received && data_ok && (lv_tick_get() - last_data_ms) < DATA_FRESH_MS) {
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

void ui_tick_anim(void) {
    if (current_screen == SCREEN_HISTORY) {
        if (lv_tick_get() - hist_pace_ms >= HIST_PACE_REFRESH_MS) update_history_pace();
        return;
    }
    if (current_screen != SCREEN_USAGE) return;
    update_view_state();
    if (view_state == 1) splash_mini_tick();   // animate the sleeping creature on the idle screen

    uint32_t now = lv_tick_get();

    // Title clock: once the daemon has sent wall-clock time, replace "Usage" with
    // the live time, advanced locally so it ticks every minute between payloads.
    if (clock_base_epoch > 0) {
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
static void apply_battery_visibility(void) {
    if (!battery_img) return;
    if (current_screen == SCREEN_SPLASH) lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    else                                  lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
}

// Tap cycles splash → usage → history → splash.
static void global_click_cb(lv_event_t* e) {
    (void)e;
    switch (current_screen) {
    case SCREEN_SPLASH:  ui_show_screen(SCREEN_USAGE);   break;
    case SCREEN_USAGE:   ui_show_screen(SCREEN_HISTORY); break;
    case SCREEN_HISTORY: ui_show_screen(SCREEN_MAXING);  break;
    default:             ui_show_screen(SCREEN_SPLASH);  break;
    }
}

void ui_show_screen(screen_t screen) {
    lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(history_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(maxing_container, LV_OBJ_FLAG_HIDDEN);
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:  splash_show(); break;
    case SCREEN_USAGE:   lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_HISTORY:
        lv_obj_clear_flag(history_container, LV_OBJ_FLAG_HIDDEN);
        update_history_pace();
        break;
    case SCREEN_MAXING:
        lv_obj_clear_flag(maxing_container, LV_OBJ_FLAG_HIDDEN);
        break;
    default: break;
    }

    splash_mascot_set_visible(screen != SCREEN_SPLASH);
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
