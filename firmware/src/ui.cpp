#include "ui.h"
#include "splash.h"
#include "idle.h"
#include <lvgl.h>
#include <time.h>
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
// All user-visible phrases live in strings.h (RU/EN, runtime-switched)
#include "lang.h"
#define COL_BG        THEME_BG
#define COL_PANEL     THEME_PANEL
#define COL_TEXT      THEME_TEXT
#define COL_DIM       THEME_DIM
#define COL_ACCENT    THEME_ACCENT
#define COL_GREEN     THEME_GREEN
#define COL_AMBER     THEME_AMBER
#define COL_RED       THEME_RED
#define COL_BLUE      THEME_BLUE
#define COL_YELLOW    THEME_YELLOW
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
static lv_obj_t* lbl_pair1, *lbl_pair2, *lbl_pair3;   // its lines (restamped on language change)
static lv_obj_t* bar_session;
static lv_obj_t* lbl_session_pct;
static lv_obj_t* lbl_session_label;
static lv_obj_t* lbl_session_reset;
static lv_obj_t* bar_weekly;
static lv_obj_t* lbl_weekly_pct;
static lv_obj_t* lbl_weekly_label;
static lv_obj_t* lbl_weekly_reset;
// Model-scoped weekly limit (Fable) — a second pill inside panel_weekly next
// to the "Неделя" one; the scoped reset matches the weekly one, so it needs
// no reset line of its own.
static lv_obj_t* pill_fable = nullptr;
static lv_obj_t* panel_session = nullptr;
static lv_obj_t* panel_weekly = nullptr;
// Enterprise-only widgets inside panel_session
static lv_obj_t* lbl_session_pct_sym = nullptr;  // "%" in smaller font
static lv_obj_t* lbl_spending_desc = nullptr;     // "of your monthly budget"
static lv_obj_t* lbl_spending_status = nullptr;   // "Under pace" / "On pace" / "Over pace"
static lv_obj_t* lbl_anim;      // status line: connection state + whimsical idle
static lv_obj_t* lbl_sessions;  // bottom-left "·N" active-session counter

// ---- Battery indicator (shared, on top) ----
static lv_obj_t* battery_img;
static lv_obj_t* battery_lbl;   // numeric percent left of the icon
static lv_obj_t* logo_img;
static lv_image_dsc_t battery_dscs[5];  // empty, low, medium, full, charging

// ---- Live-data freshness → which usage sub-view to show ----
// usage panels when data is flowing, an idle "Zzz" screen when the host is
// connected but no usage update landed within DATA_FRESH_MS, the pairing hint
// when BLE is down. Re-evaluated every loop in ui_tick_anim().
static lv_obj_t* idle_group;            // the "Zzz" idle screen
static lv_obj_t* attention_group;       // the "Claude is waiting for you" view
static lv_obj_t* lbl_attention;         // its caption (text/color vary by type)
static lv_obj_t* lbl_attn_ctx;          // event context line under the header
static lv_obj_t* mini_creature;         // the single shared mini creature canvas
static bool      attention_active = false;
static uint8_t   attention_type   = ATTN_INPUT;
static char      attention_project[97] = "";   // context line while active (mirrors UsageData::notify_project)
static uint32_t  attention_since  = 0;

// Everything type-specific in one row (index = ATTN_* - 1): the waiting
// states nag for 2 min, informational ones dismiss themselves quickly.
// The caption/status texts live in strings.h so they follow the language.
struct AttnStyle {
    const char* anim;      // mini-creature animation
    lv_color_t  color;     // caption color
    uint32_t    timeout_ms;
};
static const AttnStyle ATTN_STYLES[7] = {
    { "idle look around",    COL_AMBER,  120000 },  // ATTN_INPUT
    { "expression surprise", COL_AMBER,  120000 },  // ATTN_PERM
    { "dance bounce",        COL_GREEN,  30000  },  // ATTN_DONE
    { "dance sway",          COL_BLUE,   120000 },  // ATTN_CAL
    { "expression surprise", COL_YELLOW, 120000 },  // ATTN_CAL_START
    { "expression surprise", COL_RED,    30000  },  // ATTN_LIMIT
    { "expression wink",     COL_GREEN,  30000  },  // ATTN_RESET
};
static inline const AttnStyle& attn_style(void) { return ATTN_STYLES[attention_type - 1]; }
static uint32_t  last_data_ms = 0;      // lv_tick when the last valid usage update landed
static bool      data_received = false; // any valid update since boot
static int       view_state = -1;       // -1 unknown / 0 pair / 1 idle / 2 usage / 3 attention
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


static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

// "Сброс через 2ч 49м (21:00)" — the countdown plus, when the daemon supplied
// it, the wall-clock moment in parentheses so nobody has to do the math.
static void format_reset_time(int mins, const char* at, char* buf, size_t len) {
    char when[24] = "";
    if (at && at[0]) snprintf(when, sizeof(when), " (%s)", at);
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, S->reset_m, mins, when);
    } else if (mins < 1440) {
        snprintf(buf, len, S->reset_hm, mins / 60, mins % 60, when);
    } else {
        snprintf(buf, len, S->reset_dh, mins / 1440, (mins % 1440) / 60, when);
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

static lv_obj_t* make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
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

    lv_obj_t* l1 = lbl_pair1 = lv_label_create(pair_group);
    lv_label_set_text(l1, S->pair1);
    lv_obj_set_style_text_font(l1, L.bt_status_font, 0);
    lv_obj_set_style_text_color(l1, COL_TEXT, 0);
    lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t* l2 = lbl_pair2 = lv_label_create(pair_group);
    lv_label_set_text(l2, S->pair2);
    lv_obj_set_style_text_font(l2, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l2, COL_DIM, 0);
    lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, 120);

    lv_obj_t* l3 = lbl_pair3 = lv_label_create(pair_group);
    lv_label_set_text(l3, S->pair3);
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
    mini_creature = splash_mini_create(idle_group, "expression sleep", 160);
    if (mini_creature) lv_obj_align(mini_creature, LV_ALIGN_CENTER, 0, -20);

    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);  // update_view_state decides
}

// "Claude is waiting for you" view — shown when the host daemon forwards an
// attention request (Claude Code hook: permission prompt / waiting for input).
// The shared mini creature is re-parented here and switched to a surprised
// animation while the view is up; a caption spells out why the device chimed.
static void build_attention_group(lv_obj_t* parent) {
    attention_group = lv_obj_create(parent);
    lv_obj_set_size(attention_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(attention_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(attention_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(attention_group, 0, 0);
    lv_obj_set_style_pad_all(attention_group, 0, 0);
    lv_obj_clear_flag(attention_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(attention_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    lbl_attention = lv_label_create(attention_group);
    lv_label_set_text(lbl_attention, S->attn_caption[0]);
    lv_obj_set_style_text_font(lbl_attention, L.bt_status_font, 0);
    lv_obj_set_style_text_color(lbl_attention, COL_AMBER, 0);
    lv_obj_align(lbl_attention, LV_ALIGN_CENTER, 0, 90);

    // Event context (project name / "HH:MM meeting title") on its own line
    // right below the header, full width — up to two wrapped lines, "…" when
    // even that overflows. The header title itself stays hidden meanwhile.
    lbl_attn_ctx = lv_label_create(attention_group);
    lv_obj_set_style_text_font(lbl_attn_ctx, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_attn_ctx, COL_DIM, 0);
    lv_obj_set_style_text_align(lbl_attn_ctx, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lbl_attn_ctx, LV_LABEL_LONG_DOT);
    lv_obj_set_size(lbl_attn_ctx, L.scr_w - 2 * L.margin, 80);
    lv_obj_align(lbl_attn_ctx, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_add_flag(attention_group, LV_OBJ_FLAG_HIDDEN);  // update_view_state decides
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
    lv_label_set_text(lbl_title, S->title);
    lv_obj_set_style_text_font(lbl_title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 16, L.title_y);

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

    panel_session = make_usage_panel(usage_group, L.content_y, S->pill_session,
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset);

    // Enterprise-only overlays inside panel_session — hidden until enterprise data arrives
    lbl_session_pct_sym = lv_label_create(panel_session);
    lv_label_set_text(lbl_session_pct_sym, "%");
    lv_obj_set_style_text_font(lbl_session_pct_sym, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_session_pct_sym, COL_TEXT, 0);
    lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_desc = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_desc, S->of_monthly_budget);
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
                     L.content_y + L.usage_panel_h + L.usage_panel_gap, S->pill_weekly,
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset);
    // Recolor enabled so enterprise period box can color pace and reset separately
    lv_label_set_recolor(lbl_weekly_reset, true);

    // Fable pill — hidden until the daemon reports a model-scoped limit.
    // Slimmer than the "Неделя" pill so it doesn't crowd the big percent.
    pill_fable = make_pill(panel_weekly, "");
    lv_obj_set_style_text_font(pill_fable, &font_styrene_20, 0);
    lv_obj_set_style_pad_left(pill_fable, 12, 0);
    lv_obj_set_style_pad_right(pill_fable, 12, 0);
    lv_obj_set_style_pad_top(pill_fable, 4, 0);
    lv_obj_set_style_pad_bottom(pill_fable, 4, 0);
    lv_label_set_recolor(pill_fable, true);
    lv_obj_add_flag(pill_fable, LV_OBJ_FLAG_HIDDEN);

    build_pair_group(usage_container);
    build_idle_group(usage_container);
    build_attention_group(usage_container);

    // Status line — always visible on the usage view. Driven by ui_tick_anim().
    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, &font_mono_32, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -15);

    // Active-session counter — pinned to the bottom-left corner, independent
    // of the centered status text. Shown only on the live usage view.
    lbl_sessions = lv_label_create(usage_container);
    lv_label_set_text(lbl_sessions, "");
    lv_obj_set_style_text_font(lbl_sessions, &font_mono_32, 0);
    lv_obj_set_style_text_color(lbl_sessions, COL_DIM, 0);
    lv_obj_align(lbl_sessions, LV_ALIGN_BOTTOM_LEFT, L.margin, -15);
    lv_obj_add_flag(lbl_sessions, LV_OBJ_FLAG_HIDDEN);
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

    logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, &logo_dsc);
    lv_obj_set_pos(logo_img, L.margin, L.title_y - 10);

    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    lv_obj_set_pos(battery_img, L.scr_w - 48 - L.margin, L.title_y);

    battery_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(battery_lbl, &font_styrene_16, 0);
    lv_obj_set_style_text_color(battery_lbl, COL_DIM, 0);
    lv_label_set_text(battery_lbl, "");
}

static char data_err[8] = "";   // daemon error beat code; "" = data flows fine
static int  active_sessions = -1;   // working Claude sessions; -1 = not reported

// Bottom-left session counter: "·N" for N >= 1 on the live usage view only.
static void apply_sessions_label(void) {
    if (!lbl_sessions) return;
    if (view_state == 2 && active_sessions >= 1) {
        lv_label_set_text_fmt(lbl_sessions, "\xC2\xB7%d", active_sessions);
        lv_obj_clear_flag(lbl_sessions, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(lbl_sessions, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_set_data_error(const char* code) {
    strlcpy(data_err, code ? code : "", sizeof(data_err));
    if (!data_err[0]) return;
    // Expire the freshness window right away so update_view_state flips to
    // the idle view on the next tick instead of after DATA_FRESH_MS.
    if (data_received) last_data_ms = lv_tick_get() - DATA_FRESH_MS;
}

void ui_update(const UsageData* data) {
    if (!data->valid) return;
    data_err[0] = '\0';             // a good payload clears any error beat
    active_sessions = data->active_sessions;
    apply_sessions_label();
    last_data_ms = lv_tick_get();   // a valid usage update just landed → dot goes green
    data_received = true;

    if (data->clock_epoch > 0) {    // daemon supplied wall-clock time → drive the title clock
        clock_base_epoch = data->clock_epoch;
        clock_base_ms = last_data_ms;
        clock_fmt = data->clock_fmt;
    } else if (clock_base_epoch != 0) {   // clock turned off daemon-side → revert title to "Usage"
        clock_base_epoch = 0;
        clock_last_min = -1;
        lv_label_set_text(lbl_title, "Лимиты");
    }

    int s_pct = (int)(data->session_pct + 0.5f);

    if (data->enterprise) {
        // Spending box: big number-only label + small "%" symbol + desc + pace
        lv_obj_set_style_text_font(lbl_session_pct, &font_tiempos_56, 0);
        lv_label_set_text(lbl_session_label, S->spending);
        lv_obj_add_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status,   LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_style_text_font(lbl_session_pct, &font_styrene_48, 0);
        lv_label_set_text(lbl_session_label, S->pill_session);
        lv_obj_clear_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
    }

    char buf[96];   // UTF-8 Cyrillic is 2 bytes/char — keep headroom for the recolor markup

    // Pace vars used in both enterprise blocks below
    const char* pace_text = S->pace_under;
    lv_color_t  pace_color = COL_GREEN;
    const char* pace_hex   = "788c5d";   // matches THEME_GREEN
    if (data->session_pct > (float)data->time_pct + 15.0f) {
        pace_text = S->pace_over;  pace_color = COL_RED;   pace_hex = "c0392b";
    } else if (data->session_pct > (float)data->time_pct - 15.0f) {
        pace_text = S->pace_on; pace_color = COL_AMBER; pace_hex = "d97757";
    }

    if (data->enterprise) {
        lv_label_set_text_fmt(lbl_session_pct, "%d", s_pct);
        lv_obj_align_to(lbl_session_pct_sym, lbl_session_pct,
                        LV_ALIGN_OUT_RIGHT_TOP, 4, 12);
    } else {
        lv_label_set_text_fmt(lbl_session_pct, "%d%%", s_pct);
        format_reset_time(data->session_reset_mins, data->session_reset_at, buf, sizeof(buf));
        lv_label_set_text(lbl_session_reset, buf);
    }

    lv_bar_set_value(bar_session, s_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_session, pct_color(data->session_pct), LV_PART_INDICATOR);

    if (data->enterprise) {
        // Period box: time % + dynamic pace color + "Resets <date>" label
        lv_label_set_text(lbl_weekly_label, S->pill_period);
        lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", data->time_pct);
        lv_bar_set_value(bar_weekly, data->time_pct, LV_ANIM_ON);
        lv_color_t bar_pace = (data->session_pct <= (float)data->time_pct) ? COL_GREEN :
                              (data->session_pct <= (float)data->time_pct + 15.0f) ? COL_AMBER :
                              COL_RED;
        lv_obj_set_style_bg_color(bar_weekly, bar_pace, LV_PART_INDICATOR);
        snprintf(buf, sizeof(buf), S->ent_reset_fmt,
                 pace_hex, pace_text, data->reset_date);
        lv_label_set_text(lbl_weekly_reset, buf);
    } else {
        int w_pct = (int)(data->weekly_pct + 0.5f);
        lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", w_pct);
        lv_bar_set_value(bar_weekly, w_pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(bar_weekly, pct_color(data->weekly_pct), LV_PART_INDICATOR);
        format_reset_time(data->weekly_reset_mins, data->weekly_reset_at, buf, sizeof(buf));
        lv_label_set_text(lbl_weekly_reset, buf);
    }

    if (pill_fable) {
        if (!data->enterprise && data->fable_pct >= 0) {
            // Name dim, percent in the threshold color (mirrors pct_color).
            const char* hex = data->fable_pct >= 80 ? "c0392b" :
                              data->fable_pct >= 50 ? "d97757" : "788c5d";
            lv_label_set_text_fmt(pill_fable, "#b0aea5 %s# #%s %d%%#",
                                  data->fable_name, hex, data->fable_pct);
            lv_obj_align_to(pill_fable, lbl_weekly_label,
                            LV_ALIGN_OUT_LEFT_MID, -10, 0);
            lv_obj_clear_flag(pill_fable, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(pill_fable, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// Pick the usage-view sub-screen: pairing hint (BLE down), the idle "Zzz" screen
// (connected but data has gone stale), or the live usage panels. Only re-lays-out
// on an actual change. The animated status line stays visible everywhere — it
// reads "Listening…" on the idle screen, keeping it alive rather than frozen.
// While the attention view is up the header title is hidden (logo + battery
// stay); the event context renders full-width on lbl_attn_ctx instead — the
// header slot is too narrow between the two corner icons.
static void attention_style_title(void) {
    if (lbl_title) lv_obj_add_flag(lbl_title, LV_OBJ_FLAG_HIDDEN);
    if (!lbl_attn_ctx) return;
    if (attention_project[0]) {
        lv_label_set_text(lbl_attn_ctx, attention_project);
        lv_obj_clear_flag(lbl_attn_ctx, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(lbl_attn_ctx, LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_view_state(void) {
    if (!usage_group || !pair_group || !idle_group || !attention_group) return;
    int v;
    if (!s_ble_connected) {
        v = 0;  // pairing hint
        attention_active = false;   // stale without a live host
    } else if (attention_active) {
        v = 3;  // "Claude is waiting for you"
    } else if (data_received && (lv_tick_get() - last_data_ms) < DATA_FRESH_MS) {
        v = 2;  // live usage
    } else {
        v = 1;  // idle / Zzz
    }
    if (v == view_state) return;
    // The mini creature is a singleton — hand it to whichever view needs it.
    // The "Лимиты" header title is irrelevant while the attention view is up
    // (its caption carries the message), so hide it for the duration.
    if (mini_creature) {
        if (v == 3) {
            lv_obj_set_parent(mini_creature, attention_group);
            splash_mini_set_anim(attn_style().anim);
            // -16 (not the idle view's -20): leaves room for two wrapped
            // context lines above without touching the caption below.
            lv_obj_align(mini_creature, LV_ALIGN_CENTER, 0, -16);
        } else if (view_state == 3) {
            lv_obj_set_parent(mini_creature, idle_group);
            splash_mini_set_anim("expression sleep");
            lv_obj_align(mini_creature, LV_ALIGN_CENTER, 0, -20);
        }
    }
    // While the attention view is up, the header shows the PROJECT the event
    // belongs to (or nothing if unknown) instead of the usual "Лимиты" title.
    if (lbl_title) {
        if (v == 3) {
            attention_style_title();
        } else if (view_state == 3) {
            lv_label_set_text(lbl_title, "Лимиты");
            lv_obj_set_style_text_font(lbl_title, &font_tiempos_56, 0);
            lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
            lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 16, L.title_y);
            clock_last_min = -1;   // let an active title clock repaint itself
            lv_obj_clear_flag(lbl_title, LV_OBJ_FLAG_HIDDEN);
        }
    }
    view_state = v;
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(attention_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(v == 0 ? pair_group : v == 1 ? idle_group :
                      v == 3 ? attention_group : usage_group,
                      LV_OBJ_FLAG_HIDDEN);
    apply_sessions_label();
}

void ui_tick_anim(void) {
    if (current_screen != SCREEN_USAGE) return;
    if (attention_active &&
        lv_tick_get() - attention_since >= attn_style().timeout_ms) {
        attention_active = false;   // nobody came — stop nagging
    }
    update_view_state();
    if (view_state == 1 || view_state == 3) splash_mini_tick();  // animate the mini creature

    uint32_t now = lv_tick_get();

    // Title clock: once the daemon has sent wall-clock time, replace "Usage" with
    // the live time, advanced locally so it ticks every minute between payloads.
    // Skipped while the attention view owns the header (it shows the project).
    if (clock_base_epoch > 0 && view_state != 3) {
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
        anim_msg_idx = (anim_msg_idx + 1) % S->word_count;
        anim_msg_start = now;
    }

    // While every session is resting the spinner freezes on its calm base
    // glyph — a pulsing star reads as "working", which would be a lie.
    bool resting = (view_state == 2 && active_sessions == 0);
    if (resting) {
        anim_phase = 0;
        anim_spinner_idx = 0;
        if (now - anim_last_ms < 1000) return;   // still refresh the text ~1/s
        anim_last_ms = now;
    } else {
        if (now - anim_last_ms < spinner_ms[anim_spinner_idx]) return;
        anim_last_ms = now;
        anim_phase = (anim_phase + 1) % SPINNER_PHASES;
        anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                        : (SPINNER_PHASES - anim_phase);
    }

    // Status text by priority. Whimsical messages only when connected & settled.
    const char* text;
    if (!s_ble_connected) {
        text = S->st_waiting;          // advertising / waiting for a host connection
    } else if (view_state == 3) {      // attention — spell out why the device chimed
        text = S->attn_status[attention_type - 1];
    } else if (view_state == 1 && data_err[0]) {  // idle with a known cause — name it
        text = !strcmp(data_err, "auth")  ? S->err_auth :
               !strcmp(data_err, "token") ? S->err_token :
               !strcmp(data_err, "rate")  ? S->err_rate :
               !strcmp(data_err, "net")   ? S->err_net : S->err_api;
    } else if (view_state == 1) {      // idle — alternate so it reads as alive AND data-less
        text = (anim_msg_idx & 1) ? S->st_no_data : S->st_listening;
    } else if (now - connected_at_ms < 5000) {
        text = S->st_connected;
    } else if (active_sessions == 0) {
        text = S->st_resting;          // no Claude session is doing anything
    } else {
        text = S->words[anim_msg_idx % S->word_count];
    }

    // All states share the whimsical style: "<glyph> <Title-case word>…".
    // The active-session count lives in its own bottom-left label. Skip the
    // set_text when nothing changed — LVGL invalidates (and re-flushes over
    // QSPI) the label area on every set, which matters while resting, where
    // the composed text is identical second after second.
    static char buf[96];
    static char last_buf[96] = "";
    snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
             spinner_frames[anim_spinner_idx], text);
    if (strcmp(buf, last_buf) != 0) {
        strcpy(last_buf, buf);
        lv_label_set_text(lbl_anim, buf);
    }
}

static screen_t prev_non_splash_screen = SCREEN_USAGE;
static void apply_battery_visibility(void) {
    if (!battery_img) return;
    if (current_screen == SCREEN_SPLASH) {
        lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
        if (battery_lbl) lv_obj_add_flag(battery_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
        if (battery_lbl) lv_obj_clear_flag(battery_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

static void global_click_cb(lv_event_t* e) {
    (void)e;
    if (attention_active) {   // first tap acknowledges the attention view
        attention_active = false;
        update_view_state();
        return;
    }
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

void ui_show_attention(uint8_t type, const char* project) {
    if (type < ATTN_INPUT || type > ATTN_RESET) return;
    idle_note_activity();               // wake the panel if it faded out
    // Re-style the view even if it's already up (a new event may differ).
    bool was_active = attention_active;
    attention_active = true;
    attention_type   = type;
    strlcpy(attention_project, project ? project : "", sizeof(attention_project));
    if (lbl_attention) {
        lv_label_set_text(lbl_attention, S->attn_caption[attention_type - 1]);
        lv_obj_set_style_text_color(lbl_attention, attn_style().color, 0);
    }
    if (was_active) {   // already on the view — update_view_state won't re-enter
        if (mini_creature) splash_mini_set_anim(attn_style().anim);
        attention_style_title();   // header project may differ between events too
    }
    attention_since = lv_tick_get();
    if (current_screen != SCREEN_USAGE) ui_show_screen(SCREEN_USAGE);
    update_view_state();
}

void ui_hide_attention(void) {
    if (!attention_active) return;
    attention_active = false;
    update_view_state();
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

void ui_set_lang(const char* lang) {
    if (!strings_set_lang(lang)) return;
    // Most labels are rewritten by the next ui_update()/status tick; restamp
    // only the ones that aren't.
    if (lbl_pair1) {
        lv_label_set_text(lbl_pair1, S->pair1);
        lv_label_set_text(lbl_pair2, S->pair2);
        lv_label_set_text(lbl_pair3, S->pair3);
    }
    // The weekly pill is only rewritten by Enterprise updates ("Период") —
    // in the Pro/Max flow it keeps its creation-time text, so restamp it
    // here (a following Enterprise update overwrites it anyway).
    if (lbl_weekly_label) lv_label_set_text(lbl_weekly_label, S->pill_weekly);
    // Header: skip while the attention view owns it or the clock ticks in it.
    if (lbl_title && view_state != 3 && clock_base_epoch == 0)
        lv_label_set_text(lbl_title, S->title);
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
    if (battery_lbl) {
        // Numeric percent, charging included; hidden only when the PMU
        // doesn't report a level. Re-align after the text: the label grows
        // leftwards from the icon.
        if (percent >= 0) lv_label_set_text_fmt(battery_lbl, "%d%%", percent);
        else              lv_label_set_text(battery_lbl, "");
        lv_obj_align_to(battery_lbl, battery_img, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    }
    apply_battery_visibility();
}
