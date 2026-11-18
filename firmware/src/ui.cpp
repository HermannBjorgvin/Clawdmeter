#include "ui.h"
#include "performer.h"
#include "mascot.h"
#include "clawd_anim.h"
#include "clawd_policy.h"
#include "minimal_mode.h"
#include <lvgl.h>
#include <time.h>
#include <stdio.h>
#include <Arduino.h>
#include "logo.h"
#include "clawd_still.h"
#include "icons.h"
#include "hal/board_caps.h"
#include "layout.h"
#include "ui_text.h"
#include "pace.h"
#include "fable.h"

// Where everything goes, from the panel's shape (layout.h). Computed once in
// ui_init() and treated as const for the rest of the program; a new panel
// size never needs an edit to the screen builders below.
static Layout L = {};

static_assert(LAYOUT_LOGO_SLOT == LOGO_HEIGHT && LAYOUT_LOGO_SLOT_SMALL == LOGO_SMALL_HEIGHT, "logo slot");
static_assert(LAYOUT_LOGO_W == CLAWD_STILL_W && LAYOUT_LOGO_W_SMALL == CLAWD_STILL_SMALL_W, "corner Clawd");
static_assert(LAYOUT_BATT_W == ICON_BATTERY_W && LAYOUT_BATT_W_SMALL == ICON_BATTERY_SMALL_W, "battery icon");

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

// Colours set once when a widget is built are registered here, so a new
// palette from the app (ui_apply_theme) repaints them in place. Colours that
// follow the data (bar fills, the pace text) are recomputed instead.
enum ThemedPart : uint8_t { PART_BG, PART_TEXT, PART_IMAGE };
struct Themed { lv_obj_t* obj; ThemeColor tok; ThemedPart part; lv_style_selector_t sel; };
static Themed themed[40];
static int    n_themed = 0;

static void paint(const Themed& t) {
    switch (t.part) {
    case PART_TEXT:  lv_obj_set_style_text_color(t.obj, theme_color(t.tok), t.sel); break;
    case PART_BG:    lv_obj_set_style_bg_color(t.obj, theme_color(t.tok), t.sel); break;
    case PART_IMAGE: lv_obj_set_style_image_recolor(t.obj, theme_color(t.tok), t.sel); break;
    }
}
static void themed_color(lv_obj_t* obj, ThemeColor tok, ThemedPart part, lv_style_selector_t sel) {
    if (n_themed < (int)(sizeof(themed) / sizeof(themed[0]))) themed[n_themed++] = { obj, tok, part, sel };
    else Serial.println("ui: themed[] is full; this widget won't follow theme changes");
    paint({ obj, tok, part, sel });
}
static void themed_bg(lv_obj_t* obj, ThemeColor tok, lv_style_selector_t sel = 0) { themed_color(obj, tok, PART_BG, sel); }
static void themed_text(lv_obj_t* obj, ThemeColor tok) { themed_color(obj, tok, PART_TEXT, 0); }
// A white-tinted icon (firmware CLAUDE.md gotcha 12) drawn in a token.
static void themed_icon(lv_obj_t* obj, ThemeColor tok) {
    lv_obj_set_style_image_recolor_opa(obj, LV_OPA_COVER, 0);
    themed_color(obj, tok, PART_IMAGE, 0);
}

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
static lv_obj_t* pair_lines[4]; // its lines: heading, two steps, the hold note
static PairHint  pair_hint = PAIR_HINT_NORMAL;
static uint32_t  pair_hint_since = 0;
#define PAIR_CLEARED_SHOW_MS 8000   // "Ready to pair" stays this long
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
static lv_obj_t* lbl_spending_desc = nullptr;     // "of spending limit"
static lv_obj_t* lbl_spending_status = nullptr;   // "Under pace" / "On pace" / "Over pace"
static lv_obj_t* lbl_anim;      // status line: connection state + whimsical idle
// Codex (a trial): the Resets card is drawn into panel_weekly, over its own
// widgets (draw_resets_card): a header row, then a grid of date chips.
#define MAX_RESET_CHIPS 6
static lv_obj_t* resets_group = nullptr;   // the card's inside, its widgets' parent
static lv_obj_t* resets_title;
static lv_obj_t* resets_pill;              // the count pill: ticket + number
static lv_obj_t* resets_ticket;
static lv_obj_t* resets_count;
static lv_obj_t* resets_note;              // "No resets available" in the first chip row's place
static lv_obj_t* resets_chip[MAX_RESET_CHIPS];
static lv_obj_t* resets_chip_text[MAX_RESET_CHIPS];
static lv_image_dsc_t ticket_dsc;

// ---- Battery indicator (shared, on top) ----
static lv_obj_t* battery_img;
static lv_obj_t* logo_img;
static lv_image_dsc_t battery_dscs[6];  // empty, low, medium, full, charging, plug-zap (no battery)

// ---- Live-data freshness → which usage sub-view to show ----
// Live meters when data is flowing; the same meters dimmed when the host is
// connected but no usage update landed within DATA_FRESH_MS (stale: the last
// numbers stay, visibly not live, and the status line says so); the pairing
// hint when BLE is down. The corner Clawd stays in his seat in all three:
// there is only ever one Clawd. Re-evaluated every loop in ui_tick_anim().
#define STALE_OPA LV_OPA_40
static uint32_t  last_data_ms = 0;      // lv_tick when the last valid usage update landed
static bool      data_received = false; // any valid update since boot
static bool      data_ok = true;        // last payload's ok flag; a {"ok":false} beat = "no fresh data"
static int       view_state = -1;       // -1 unknown / 0 pair / 1 stale / 2 live
static const uint32_t DATA_FRESH_MS = 90000;  // usage counts as "live" within this window (daemon sends ~60s)

// ---- Shared ----
static lv_image_dsc_t logo_dsc;
static bool     s_ble_connected = false;   // cached BLE connection state
static uint32_t connected_at_ms = 0;       // when we last entered CONNECTED ("Connected" dwell)

// Animation state
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;    // counts verb changes (the stale line alternates on it)
static uint8_t anim_verb_idx = 0;   // the verb shown: the next one that fits the line
static uint32_t anim_msg_start = 0;
static const char* frozen_shown = nullptr;   // spinner held still: the word on screen

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

// The first verb from `i` on that fits the status line as "<glyph> <verb>…";
// a narrow panel skips the long ones rather than clipping them.
static uint8_t fitting_verb(uint8_t i) {
    for (uint8_t n = 0; n < ANIM_MSG_COUNT; n++, i = (i + 1) % ANIM_MSG_COUNT) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6", spinner_frames[1], anim_messages[i]);
        if (layout_text_width(L.status_font, buf) <= L.status_w) return i;
    }
    return i;
}

static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

// ---- Pace marker (device setting `pm`, pace.h). Off: the bars use
// pct_color(). On: each Pro/Max bar gets a thin ivory line at the elapsed
// share of its window and the fill follows pace_level(). A bar with no reset
// time hides its line and keeps pct_color(). Spend-capped accounts are
// untouched: their Period bar already is the elapsed share. On a mono panel
// every fill prints as ink, so the line gets a paper edge (make_pace_tick).
static bool pace_marker = false;
static lv_obj_t* tick_session = nullptr;
static lv_obj_t* tick_weekly  = nullptr;
static UsageData last_data;                 // recoloured when the setting changes
static bool have_last_data = false;

static lv_color_t bar_color(float used_pct, int reset_mins, int window_mins) {
    if (pace_marker) {
        switch (pace_level(used_pct, pace_elapsed_pct(reset_mins, window_mins))) {
        case PACE_LIMIT: return COL_RED;
        case PACE_AHEAD: return COL_AMBER;
        case PACE_ON:    return COL_GREEN;
        case PACE_UNKNOWN: break;
        }
    }
    return pct_color(used_pct);
}

// A line inside the bar, its full height inside any outline (mono_panel),
// drawn over the fill. On a mono panel the fill and the line both print as
// ink, so the line gets a 1 px paper edge on each side: it reads the same
// over the fill, over the track and at the fill's end.
static lv_obj_t* make_pace_tick(lv_obj_t* bar) {
    const int border = lv_obj_get_style_border_width(bar, LV_PART_MAIN);
    const bool mono = board_caps().mono_panel;
    const int w = L.bar_h >= 20 ? 3 : 2;
    lv_obj_t* tick = lv_obj_create(bar);
    lv_obj_remove_style_all(tick);
    lv_obj_set_size(tick, mono ? w + 2 : w, L.bar_h - 2 * border);
    themed_bg(tick, TH_TEXT);
    lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tick, mono ? 0 : 1, 0);
    if (mono) {
        lv_obj_set_style_border_color(tick, COL_BG, 0);
        lv_obj_set_style_border_opa(tick, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(tick, 1, 0);
        lv_obj_set_style_border_side(tick, (lv_border_side_t)(LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT), 0);
    }
    lv_obj_clear_flag(tick, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(tick, LV_OBJ_FLAG_HIDDEN);
    return tick;
}

// Centre the line on elapsed_pct, kept clear of the bar's rounded ends and
// inside its outline; hidden while the setting is off or the elapsed share
// is unknown.
static void place_pace_tick(lv_obj_t* tick, float elapsed_pct) {
    if (!pace_marker || elapsed_pct < 0) {
        lv_obj_add_flag(tick, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_t* bar = lv_obj_get_parent(tick);
    const int bar_w = lv_obj_get_width(bar);
    const int w = lv_obj_get_width(tick);
    // The fill spans the whole bar, outline included, but children sit
    // inside the outline: x is worked out on the bar and shifted after.
    const int border = lv_obj_get_style_border_width(bar, LV_PART_MAIN);
    const int radius = lv_obj_get_style_radius(bar, LV_PART_MAIN);
    int inset = radius / 2;
    // An outline draws the rounded ends: keep clear of the whole curve.
    if (border) inset = LV_MIN(radius, lv_obj_get_height(bar) / 2);
    int x = (int)(elapsed_pct * bar_w / 100.0f + 0.5f) - w / 2;
    if (x < inset) x = inset;
    if (x > bar_w - inset - w) x = bar_w - inset - w;
    lv_obj_set_x(tick, x - border);
    lv_obj_clear_flag(tick, LV_OBJ_FLAG_HIDDEN);
}

// ---- Fable overlay (payload `wf`, fable.h). A violet strip from the left
// end of the bar that shows the week (the second card, or the first for an
// account with no 5-hour limit), FABLE_WEEKLY_SHARE_PCT % of the bar wide at
// Fable's full limit, drawn over the weekly fill and under the pace line.
// Its left end follows the bar's rounded end and its right end is straight,
// cut where Fable's share ends; it stops at the end of the weekly fill and
// there takes the fill's rounded corner. LVGL radii are uniform, so the
// shape is a rounded strip as wide as the fill inside a square container as
// wide as the overlay: a container clips its children to its rectangle with
// no extra buffer (clip_corner would render through ARGB layers on every
// redraw, which the C6's internal-RAM LVGL pool can do without). The
// straight end meets the fill directly. Its colour is the theme's (`fc`).
// Hidden without `wf`, at 0 %, on spend-capped accounts (no weekly bar) and
// on a mono panel, where it would print as the fill's ink.
struct FableOverlay { lv_obj_t* clip; lv_obj_t* strip; };
static FableOverlay fable_session = {};
static FableOverlay fable_weekly  = {};

static lv_obj_t* make_plain(lv_obj_t* parent, int h) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, 0, h);
    lv_obj_clear_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    return o;
}

static FableOverlay make_fable_overlay(lv_obj_t* bar) {
    const int h = L.bar_h - 2 * lv_obj_get_style_border_width(bar, LV_PART_MAIN);
    FableOverlay f;
    f.clip = make_plain(bar, h);
    f.strip = make_plain(f.clip, h);
    lv_obj_set_style_bg_opa(f.strip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(f.strip, lv_obj_get_style_radius(bar, LV_PART_INDICATOR), 0);
    lv_obj_add_flag(f.clip, LV_OBJ_FLAG_HIDDEN);
    return f;
}

// `value`: the bar's value (its weekly %), whose fill the overlay stays in.
static void place_fable(const FableOverlay& f, int fable_pct, int value) {
    const int bar_w = lv_obj_get_width(lv_obj_get_parent(f.clip));
    const int fill_w = fable_fill_w(bar_w, value);
    // A fill narrower than its two rounded ends is drawn by lv_bar clipped
    // to the track's corners (or not at all at 1 px), which the strip can't
    // follow: no overlay until the fill is that wide. A device-only limit;
    // the app's strip follows any fill.
    const int radius = lv_obj_get_style_radius(f.strip, LV_PART_MAIN);
    const bool hide = board_caps().mono_panel || fill_w < 2 * radius;
    const int w = hide ? 0 : fable_overlay_w(fable_pct, value, bar_w, fill_w);
    if (w < 1) {
        lv_obj_add_flag(f.clip, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_set_width(f.clip, w);
    lv_obj_set_width(f.strip, fill_w);
    lv_obj_set_style_bg_color(f.strip, lv_color_hex(theme_fable_hex()), 0);   // `fc`
    lv_obj_clear_flag(f.clip, LV_OBJ_FLAG_HIDDEN);
}

static void hide_fable(const FableOverlay& f) { lv_obj_add_flag(f.clip, LV_OBJ_FLAG_HIDDEN); }

// `none`: what a missing reset time means (-1 in the payload): the session
// window hasn't started, the weekly one is unknown.
static void format_reset_time(int mins, char* buf, size_t len, const char* none) {
    if (mins < 0) {
        snprintf(buf, len, "%s", none);
    } else if (mins < 60) {
        snprintf(buf, len, "Resets in %dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "Resets in %dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "Resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    themed_bg(panel, TH_PANEL);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, L.pad_x, 0);
    lv_obj_set_style_pad_right(panel, L.pad_x, 0);
    lv_obj_set_style_pad_top(panel, L.pad_y, 0);
    lv_obj_set_style_pad_bottom(panel, L.pad_y, 0);
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
    themed_bg(bar, TH_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    if (board_caps().mono_panel) {   // the dark track prints as paper: outline it
        lv_obj_set_style_border_color(bar, COL_TEXT, LV_PART_MAIN);
        lv_obj_set_style_border_width(bar, 2, LV_PART_MAIN);
    }
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
    // mono_panel: the dark fill would print as paper, leaving bare text, so
    // the pill inverts to paper text on a fixed ink grey (THEME_MONO_PILL
    // says why that grey; a mono panel never takes a new palette).
    if (board_caps().mono_panel) {
        themed_text(lbl, TH_BG);
        lv_obj_set_style_bg_color(lbl, THEME_MONO_PILL, 0);
    } else {
        themed_text(lbl, TH_TEXT);
        themed_bg(lbl, TH_BAR_BG);
    }
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
        init_icon_dsc_rgb565a8(&battery_dscs[5], ICON_PLUG_ZAP_SMALL_W, ICON_PLUG_ZAP_SMALL_H, icon_plug_zap_small_data);
        return;
    }
    init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_data);
    init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_W, ICON_BATTERY_LOW_H, icon_battery_low_data);
    init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_W, ICON_BATTERY_MEDIUM_H, icon_battery_medium_data);
    init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_W, ICON_BATTERY_FULL_H, icon_battery_full_data);
    init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_W, ICON_BATTERY_CHARGING_H, icon_battery_charging_data);
    init_icon_dsc_rgb565a8(&battery_dscs[5], ICON_PLUG_ZAP_W, ICON_PLUG_ZAP_H, icon_plug_zap_data);
}

// ======== Usage Screen ========

static lv_obj_t* make_usage_panel(lv_obj_t* parent, const LayoutRect& r, const char* pill_text,
                                  lv_obj_t** out_pct, lv_obj_t** out_pill,
                                  lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, r.x, r.y, r.w, r.h);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "--%");
    lv_obj_set_style_text_font(*out_pct, L.pct_font, 0);
    themed_text(*out_pct, TH_TEXT);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, L.bar_y, r.w - 2 * L.pad_x, L.bar_h);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, L.reset_font, 0);
    themed_text(*out_reset, TH_DIM);
    lv_obj_set_pos(*out_reset, 0, L.reset_y);

    return panel;
}

// Pairing hint — shown when disconnected so the screen isn't empty and the
// user knows how to (re)pair. Boards with a PWR button name the 3-second
// release gesture (main.cpp pair_tick); boards without one (LCD-4) name the
// 5 s touch hold on this hint that clears an old pairing (pair_hold.h), and
// only those lines change while the hold runs and after a clear.
static void apply_pair_text(void) {
    if (!pair_lines[0]) return;
    const bool pwr = board_caps().has_pwr_button;
    const char* const* lines = pwr ? PAIR_TEXT_PWR : PAIR_TEXT_TOUCH;
    const char* note = "";
    // Only the touch hold talks back; the PWR hint stays as it is.
    if (!pwr && pair_hint == PAIR_HINT_HOLDING) note = PAIR_TEXT_HOLDING;
    else if (!pwr && pair_hint == PAIR_HINT_CLEARED) lines = PAIR_TEXT_CLEARED;
    for (int i = 0; i < 3; i++) lv_label_set_text(pair_lines[i], lines[i]);
    lv_label_set_text(pair_lines[3], note);
    lv_obj_set_style_text_color(pair_lines[3], pair_hint == PAIR_HINT_NORMAL ? COL_DIM : COL_ACCENT, 0);
}

// A centred line; on panels where the steps wrap, it wraps at pair_w.
static lv_obj_t* make_pair_line(int i, ThemeColor color) {
    lv_obj_t* l = lv_label_create(pair_group);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, L.pair_font[i], 0);
    themed_text(l, color);
    if (L.pair_wrap) {
        lv_obj_set_width(l, L.pair_w);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, L.pair_line_y[i]);
    return l;
}

static void build_pair_group(lv_obj_t* parent) {
    pair_group = lv_obj_create(parent);
    lv_obj_set_size(pair_group, L.w, L.h - L.pair_y);
    lv_obj_set_pos(pair_group, 0, L.pair_y);
    lv_obj_set_style_bg_opa(pair_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pair_group, 0, 0);
    lv_obj_set_style_pad_all(pair_group, 0, 0);
    lv_obj_clear_flag(pair_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    pair_lines[0] = make_pair_line(0, TH_TEXT);
    pair_lines[1] = make_pair_line(1, TH_DIM);
    pair_lines[2] = make_pair_line(2, TH_DIM);
    pair_lines[3] = make_pair_line(3, TH_DIM);   // recoloured by apply_pair_text
    apply_pair_text();

    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);  // ui_update_ble_status decides
}

bool ui_pair_hint_visible(void) { return view_state == 0; }

void ui_set_pair_hint(PairHint hint) {
    if (hint == pair_hint) return;
    pair_hint = hint;
    pair_hint_since = lv_tick_get();
    apply_pair_text();
}

// The title's font follows the clock format (a 12-hour clock may need a
// smaller one); hidden where the header has no room for it at all.
static void apply_title_font(void) {
    const int fmt = clock_base_epoch > 0 && clock_fmt == 12 ? 1 : 0;
    const lv_font_t* f = L.title_font[fmt];
    if (!f) {
        lv_obj_add_flag(lbl_title, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_set_style_text_font(lbl_title, f, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, L.title_cx - L.w / 2, L.title_y[fmt]);
    lv_obj_clear_flag(lbl_title, LV_OBJ_FLAG_HIDDEN);
}

// A bare object for the Resets card: no theme styles, not clickable (a tap
// reaches the screen as anywhere else on the meters).
static lv_obj_t* plain_obj(lv_obj_t* parent) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
    return o;
}

// The Codex Resets card's widgets, hidden until a Codex payload with reset
// credits arrives. Geometry from layout.cpp (fit_resets); colours and text
// from draw_resets_card.
static void build_resets_card(void) {
    static const int ticket_w[] = { ICON_TICKET_20_W, ICON_TICKET_10_W, ICON_TICKET_8_W };
    static const int ticket_h[] = { ICON_TICKET_20_H, ICON_TICKET_10_H, ICON_TICKET_8_H };
    static const uint8_t* const ticket_data[] = { icon_ticket_20_data, icon_ticket_10_data, icon_ticket_8_data };
    const int t = L.resets_ticket;
    init_icon_dsc_rgb565a8(&ticket_dsc, ticket_w[t], ticket_h[t], ticket_data[t]);

    resets_group = plain_obj(panel_weekly);
    lv_obj_set_pos(resets_group, 0, 0);
    lv_obj_set_size(resets_group, L.card[1].w - 2 * L.pad_x, L.card[1].h - 2 * L.pad_y);
    lv_obj_add_flag(resets_group, LV_OBJ_FLAG_HIDDEN);

    // Header: the title on the pill text's baseline, the pill at the top
    // right 1 px down, as the first card's "Codex" pill sits.
    resets_title = lv_label_create(resets_group);
    lv_label_set_text(resets_title, L.resets_title);
    lv_obj_set_style_text_font(resets_title, L.pill_font, 0);
    lv_obj_set_pos(resets_title, 0, 1 + L.pill_pad_y);

    resets_pill = plain_obj(resets_group);
    lv_obj_set_style_bg_opa(resets_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(resets_pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_height(resets_pill, lv_font_get_line_height(L.pill_font) + 2 * L.pill_pad_y);
    resets_ticket = lv_image_create(resets_pill);
    lv_image_set_src(resets_ticket, &ticket_dsc);
    lv_obj_set_style_image_recolor_opa(resets_ticket, LV_OPA_COVER, 0);
    // The ticket stands on the number's baseline, its height the caps'.
    const int baseline = L.pill_pad_y + L.pill_font->line_height - L.pill_font->base_line;
    lv_obj_set_pos(resets_ticket, L.pill_pad_x, baseline - ticket_h[t]);
    resets_count = lv_label_create(resets_pill);
    lv_obj_set_style_text_font(resets_count, L.pill_font, 0);
    lv_obj_set_pos(resets_count, L.pill_pad_x + ticket_w[t] + L.resets_icon_gap, L.pill_pad_y);

    for (int i = 0; i < MAX_RESET_CHIPS; i++) {
        lv_obj_t* c = plain_obj(resets_group);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
        if (board_caps().mono_panel) {   // like the bars: the dark chip prints as paper, so outline it
            lv_obj_set_style_border_color(c, COL_TEXT, 0);
            lv_obj_set_style_border_width(c, 2, 0);
        }
        resets_chip_text[i] = lv_label_create(c);
        lv_obj_set_style_text_font(resets_chip_text[i], L.resets_font, 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
        resets_chip[i] = c;
    }
    resets_note = lv_label_create(resets_group);
    lv_obj_set_style_text_font(resets_note, L.resets_font, 0);
    lv_obj_set_pos(resets_note, 0,
                   L.resets_chips_y + (L.resets_chip_h - lv_font_get_line_height(L.resets_font)) / 2);
}

static void init_usage_screen(lv_obj_t* scr) {
    usage_container = lv_obj_create(scr);
    lv_obj_set_size(usage_container, L.w, L.h);
    lv_obj_set_pos(usage_container, 0, 0);
    lv_obj_set_style_bg_opa(usage_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_container, 0, 0);
    lv_obj_set_style_pad_all(usage_container, 0, 0);
    lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_SCROLLABLE);

    lbl_title = lv_label_create(usage_container);
    lv_label_set_text(lbl_title, "Usage");
    themed_text(lbl_title, TH_TEXT);
    apply_title_font();

    // Usage panels (shown when connected) live in a transparent full-size group
    // so they can be toggled against the pairing hint as one unit.
    usage_group = lv_obj_create(usage_container);
    lv_obj_set_size(usage_group, L.w, L.h);
    lv_obj_set_pos(usage_group, 0, 0);
    lv_obj_set_style_bg_opa(usage_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_group, 0, 0);
    lv_obj_set_style_pad_all(usage_group, 0, 0);
    lv_obj_clear_flag(usage_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    panel_session = make_usage_panel(usage_group, L.card[0], L.pills[0],
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset);

    // Enterprise-only overlays inside panel_session — hidden until enterprise data arrives
    lbl_session_pct_sym = lv_label_create(panel_session);
    lv_label_set_text(lbl_session_pct_sym, "%");
    lv_obj_set_style_text_font(lbl_session_pct_sym, L.reset_font, 0);
    themed_text(lbl_session_pct_sym, TH_TEXT);
    lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_desc = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_desc, "of spending limit");
    lv_obj_set_style_text_font(lbl_spending_desc, L.reset_font, 0);
    themed_text(lbl_spending_desc, TH_DIM);
    lv_obj_set_pos(lbl_spending_desc, 0, L.reset_y);
    lv_obj_add_flag(lbl_spending_desc, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_status = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_status, "");
    lv_obj_set_style_text_font(lbl_spending_status, L.pace_font, 0);
    lv_obj_set_pos(lbl_spending_status, 0, L.reset_y + 20);
    lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);

    panel_weekly = make_usage_panel(usage_group, L.card[1], L.pills[1],
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset);
    // Recolor enabled so enterprise period box can color pace and reset separately
    lv_label_set_recolor(lbl_weekly_reset, true);

    // The Fable overlay first, so the pace line draws over it.
    fable_session = make_fable_overlay(bar_session);
    fable_weekly  = make_fable_overlay(bar_weekly);
    tick_session = make_pace_tick(bar_session);
    tick_weekly  = make_pace_tick(bar_weekly);

    build_resets_card();

    build_pair_group(usage_container);

    // Status line — always visible on the usage view. Driven by ui_tick_anim().
    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, L.status_font, 0);
    themed_text(lbl_anim, TH_ACCENT);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, L.status_y);
}

// ---- Attention message on the status line (the director sequences the rest).
static bool alert_active = false;
static const char* still_shown = nullptr;   // minimal mode: status word on screen

// ======== Public API ========

void ui_init(void) {
    const BoardCaps& caps = board_caps();
    L = layout_compute({ caps.width, caps.height, caps.round_panel });

    lv_obj_t* scr = lv_screen_active();
    themed_bg(scr, TH_BG);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    // The screen is a fixed room, not a scroll view. Left scrollable (the
    // LVGL default), it grows a theme scrollbar (a faint 4 px grey bar ~6 px
    // above the bottom edge) whenever Clawd walks past an edge, e.g. on a
    // lurk trip or the screensaver's walk-off, and a drag could scroll it.
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    // Static corner mascot (see clawd_still.h): fallback when the animated
    // one can't get its buffer. Baked art, so it keeps the official orange
    // whatever TH_CLAWD is.
    if (L.small_icons) init_icon_dsc_rgb565a8(&logo_dsc, CLAWD_STILL_SMALL_W, CLAWD_STILL_SMALL_H, clawd_still_small_data);
    else               init_icon_dsc_rgb565a8(&logo_dsc, CLAWD_STILL_W, CLAWD_STILL_H, clawd_still_data);
    init_battery_icons();

    init_usage_screen(scr);

    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    lv_obj_set_pos(battery_img, L.batt_x, L.batt_y);
    // Boards without battery telemetry never show the indicator (per the HAL
    // contract).
    if (!board_caps().has_battery) {
        lv_obj_del(battery_img);
        battery_img = nullptr;
    } else {
        themed_icon(battery_img, TH_TEXT);   // white by default; readable on a light theme
    }

    // Corner mascot in the old logo slot. The still Clawd is shorter than the
    // 80/40 px slot the spark logo used; center it vertically in that slot.
    {
        const int slot  = L.small_icons ? LOGO_SMALL_HEIGHT : LOGO_HEIGHT;
        const int art_h = L.small_icons ? CLAWD_STILL_SMALL_H : CLAWD_STILL_H;
        const int top   = L.logo_y + (slot - art_h) / 2;
        const int cell  = L.small_icons ? 2 : 3;
        // His tallest act (jumping happy, the fresh-window joy) rises well
        // above the idle pose. Lower the seat just enough for it to clear the
        // top edge, without sitting on the first panel. Where the layout has
        // no room for that (240 px panels) the mascot swaps in a shorter
        // official act instead of clipping (mascot.cpp).
        const ClawdAnim* jump = clawd_anim("jumping happy");
        const int feet = jump ? corner_seat_feet(top + art_h, jump->h * cell, L.card[0].y - 4, 2)
                              : top + art_h;
        // Animated: idles, does acts, and takes walk-off/lurk trips (the
        // trips need PSRAM; without it he stays in his seat).
        // Created after the panels so he walks over the battery.
        if (!mascot_create(scr, L.logo_x, feet, cell)) {
            logo_img = lv_image_create(scr);
            lv_image_set_src(logo_img, &logo_dsc);
            lv_obj_set_pos(logo_img, L.logo_x, top);
        }
    }

    // Clawd performs in front of the meters, fully drawn, like the
    // attention walk-on (a tap sends him off). Created last so he's on top
    // of everything on the screen. His feet stand on the bottom edge of the
    // last panel, clear of the (Clawd-orange) status line.
    performer_init(scr, L.card[1].y + L.card[1].h);

    lv_obj_set_style_text_align(lbl_anim, LV_TEXT_ALIGN_CENTER, 0);
    anim_verb_idx = fitting_verb(0);
}

// A Codex account with its reset credits: the second card is the Resets
// card. Without them (or with a 5-hour window, which keeps the week there)
// the Claude layout holds.
static bool codex_resets_card(const UsageData* d) {
    return d->codex && d->weekly_only && d->resets_held >= 0;
}

// An account with no 5-hour limit and a Fable reading: the second card,
// otherwise hidden, is Fable's own meter (its % of its own limit, in the
// Fable colour, over the week's reset countdown: Fable's window is the
// week). The strip in the weekly bar goes while the card shows. Not on a
// 1-bit panel, which draws no Fable colour.
static bool fable_card(const UsageData* d) {
    return d->weekly_only && !d->codex && !d->enterprise && d->fable_pct >= 0
        && !board_caps().mono_panel;
}

// The first of `texts` that fits `w` in `font`, else the last.
static const char* first_fitting(const lv_font_t* font, const char* const* texts, int n, int w) {
    for (int i = 0; i < n - 1; i++)
        if (layout_text_width(font, texts[i]) <= w) return texts[i];
    return texts[n - 1];
}

// The weekly widgets panel_weekly shows outside the Resets card.
static void show_weekly_widgets(bool on) {
    lv_obj_t* const parts[] = { lbl_weekly_pct, lbl_weekly_label, bar_weekly, lbl_weekly_reset };
    for (lv_obj_t* o : parts) {
        if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

// Put panel_weekly back for the week: the Resets card goes.
static void hide_resets_card(void) {
    if (!resets_group || lv_obj_has_flag(resets_group, LV_OBJ_FLAG_HIDDEN)) return;
    lv_obj_add_flag(resets_group, LV_OBJ_FLAG_HIDDEN);
    show_weekly_widgets(true);
}

// The Codex Resets card, drawn into panel_weekly; ui_update and a theme
// change call it, and its whole design is here. A header pinned to the top
// like the first card's value row: "Resets expire" and a count pill with
// the ticket (dim at 0). Under it, a cols x rows grid (layout.cpp
// fit_resets) of date chips, soonest first, the soonest inverted; more
// credits than cells: one fewer date and "+N more". No credits, or no
// dates for them: a dim line in the first row's place.
static void draw_resets_card(const UsageData* d) {
    if (!resets_group) return;
    show_weekly_widgets(false);
    lv_obj_add_flag(tick_weekly, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(resets_group, LV_OBJ_FLAG_HIDDEN);
    const bool mono = board_caps().mono_panel;
    const int held = d->resets_held;
    const int inner_w = L.card[1].w - 2 * L.pad_x;

    // Header. The pill follows make_pill's colours (inverted on a mono panel).
    lv_obj_set_style_text_color(resets_title, COL_TEXT, 0);
    const ThemeColor fg = mono ? TH_BG : held ? TH_TEXT : TH_DIM;
    lv_obj_set_style_bg_color(resets_pill, mono ? THEME_MONO_PILL : COL_BAR_BG, 0);
    lv_obj_set_style_image_recolor(resets_ticket, theme_color(fg), 0);
    lv_obj_set_style_text_color(resets_count, theme_color(fg), 0);
    lv_label_set_text_fmt(resets_count, "%d", held);
    lv_obj_set_width(resets_pill, lv_obj_get_x(resets_count)
                                  + layout_text_width(L.pill_font, lv_label_get_text(resets_count)) + L.pill_pad_x);
    lv_obj_align(resets_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    // What the chips show: the dates there are, soonest first.
    const int cells = L.resets_cols * L.resets_rows;
    int dated = d->resets_expiry_n > 0 ? d->resets_expiry_n : 0;
    if (dated > held) dated = held;
    int shown = dated, more = 0;
    if (dated < held || held > cells) {
        shown = dated < cells - 1 ? dated : cells - 1;
        more = held - shown;
    }
    if (held > 0 && dated == 0) shown = more = 0;   // no dates to show: the note says so

    const int n_note = sizeof(RESETS_EMPTY) / sizeof(RESETS_EMPTY[0]);
    if (held == 0 || dated == 0) {
        const char* const* texts = held == 0 ? RESETS_EMPTY : RESETS_UNDATED;
        lv_label_set_text(resets_note, first_fitting(L.resets_font, texts, n_note, inner_w));
        lv_obj_set_style_text_color(resets_note, COL_DIM, 0);
        lv_obj_clear_flag(resets_note, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(resets_note, LV_OBJ_FLAG_HIDDEN);
    }

    const int cols = L.resets_cols, gap = L.resets_gap;
    for (int i = 0; i < MAX_RESET_CHIPS; i++) {
        lv_obj_t* c = resets_chip[i];
        lv_obj_t* label = resets_chip_text[i];
        if (i >= shown + (more ? 1 : 0)) {
            lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        // Equal cells on whole pixels; the last ends on the card's padding.
        const int row = i / cols, col = i % cols;
        const int x0 = col * (inner_w + gap) / cols;
        const int x1 = (col + 1) * (inner_w + gap) / cols - gap;
        lv_obj_set_pos(c, x0, L.resets_chips_y + row * (L.resets_chip_h + gap));
        lv_obj_set_size(c, x1 - x0, L.resets_chip_h);
        ThemeColor bg = TH_BAR_BG, text = TH_TEXT;
        bool mono_pill = false;
        if (i < shown) {
            lv_label_set_text(label, d->resets_expiry[i]);
            // The soonest, inverted. Paper text on ink prints broken in 1 bit,
            // so a mono panel gives it the pill's grey instead (make_pill).
            if (i == 0) {
                if (mono) { mono_pill = true; text = TH_BG; }
                else      { bg = TH_TEXT; text = TH_PANEL; }
            }
        } else {
            char buf[16];
            snprintf(buf, sizeof(buf), "+%d more", more);
            if (layout_text_width(L.resets_font, buf) + L.resets_chip_h / 2 > x1 - x0)
                snprintf(buf, sizeof(buf), "+%d", more);
            lv_label_set_text(label, buf);
            text = TH_DIM;
        }
        lv_obj_set_style_bg_color(c, mono_pill ? THEME_MONO_PILL : theme_color(bg), 0);
        lv_obj_set_style_text_color(label, theme_color(text), 0);
        lv_obj_center(label);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_HIDDEN);
    }
}

// The Pro/Max bars' fill colours, pace markers and Fable overlay. A Codex
// account's bars are white (TH_TEXT), marking numbers from the GPT side,
// with the pace marker in TH_DIM so it shows on the fill, and never the
// Fable overlay (Fable is Claude's).
static void apply_bar_colors(const UsageData* data) {
    const bool mono = board_caps().mono_panel;
    const ThemeColor tick = data->codex && !mono ? TH_DIM : TH_TEXT;
    lv_obj_set_style_bg_color(tick_session, theme_color(tick), 0);
    lv_obj_set_style_bg_color(tick_weekly, theme_color(tick), 0);
    if (data->codex) {
        hide_fable(fable_session);
        hide_fable(fable_weekly);
        lv_obj_set_style_bg_color(bar_session, theme_color(TH_TEXT), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(bar_weekly, theme_color(TH_TEXT), LV_PART_INDICATOR);
        const int first_window = data->weekly_only ? PACE_WEEK_MINS : PACE_SESSION_MINS;
        place_pace_tick(tick_session, pace_elapsed_pct(data->session_reset_mins, first_window));
        place_pace_tick(tick_weekly, data->weekly_only ? -1.0f
                                                       : pace_elapsed_pct(data->weekly_reset_mins, PACE_WEEK_MINS));
        return;
    }
    if (data->enterprise) {
        // Spending has no 5-hour window, and the Period bar (coloured in
        // ui_update) already is the elapsed share: no markers, no week.
        lv_obj_set_style_bg_color(bar_session, pct_color(data->session_pct), LV_PART_INDICATOR);
        place_pace_tick(tick_session, -1.0f);
        place_pace_tick(tick_weekly, -1.0f);
        hide_fable(fable_session);
        hide_fable(fable_weekly);
        return;
    }
    if (data->weekly_only) {
        // No 5-hour limit: the first card is the week, the second is hidden,
        // or Fable's (fable_card), which takes the strip's place.
        lv_obj_set_style_bg_color(bar_session,
            bar_color(data->session_pct, data->session_reset_mins, PACE_WEEK_MINS), LV_PART_INDICATOR);
        place_pace_tick(tick_session, pace_elapsed_pct(data->session_reset_mins, PACE_WEEK_MINS));
        place_pace_tick(tick_weekly, -1.0f);
        hide_fable(fable_weekly);
        if (fable_card(data)) {
            hide_fable(fable_session);
            lv_obj_set_style_bg_color(bar_weekly, lv_color_hex(theme_fable_hex()), LV_PART_INDICATOR);
        } else {
            place_fable(fable_session, data->fable_pct, (int)(data->session_pct + 0.5f));
        }
        return;
    }
    hide_fable(fable_session);
    place_fable(fable_weekly, data->fable_pct, (int)(data->weekly_pct + 0.5f));
    lv_obj_set_style_bg_color(bar_session,
        bar_color(data->session_pct, data->session_reset_mins, PACE_SESSION_MINS), LV_PART_INDICATOR);
    place_pace_tick(tick_session, pace_elapsed_pct(data->session_reset_mins, PACE_SESSION_MINS));
    lv_obj_set_style_bg_color(bar_weekly,
        bar_color(data->weekly_pct, data->weekly_reset_mins, PACE_WEEK_MINS), LV_PART_INDICATOR);
    place_pace_tick(tick_weekly, pace_elapsed_pct(data->weekly_reset_mins, PACE_WEEK_MINS));
}

// Enterprise Period box: the bar's fill and the recoloured pace + reset text.
static void apply_period_colors(const UsageData* data) {
    const float s = data->session_pct, tp = (float)data->time_pct;
    const ThemeColor bar_pace = s <= tp ? TH_GREEN : s <= tp + 15.0f ? TH_AMBER : TH_RED;
    lv_obj_set_style_bg_color(bar_weekly, theme_color(bar_pace), LV_PART_INDICATOR);
    const char* pace_text = "Under pace";
    ThemeColor  pace = TH_GREEN;
    if (s > tp + 15.0f)      { pace_text = "Over pace"; pace = TH_RED; }
    else if (s > tp - 15.0f) { pace_text = "On pace";   pace = TH_AMBER; }
    char buf[64];
    if (L.period_short)
        snprintf(buf, sizeof(buf), "#%06lx Resets %s#", (unsigned long)theme_hex(pace), data->reset_date);
    else
        snprintf(buf, sizeof(buf), "#%06lx %s# - #%06lx Resets %s#", (unsigned long)theme_hex(pace),
                 pace_text, (unsigned long)theme_hex(TH_TEXT), data->reset_date);
    lv_label_set_text(lbl_weekly_reset, buf);
}

void ui_set_pace_marker(bool on) {
    if (on == pace_marker) return;
    pace_marker = on;
    if (have_last_data) apply_bar_colors(&last_data);
}

void ui_apply_theme(void) {
    for (int i = 0; i < n_themed; i++) paint(themed[i]);
    apply_pair_text();   // its last line follows the hold state
    if (have_last_data) {
        apply_bar_colors(&last_data);
        if (codex_resets_card(&last_data)) draw_resets_card(&last_data);
        if (last_data.enterprise) apply_period_colors(&last_data);
    }
}

void ui_update(const UsageData* data) {
    if (!data->valid) return;
    data_ok = data->ok;
    if (!data->ok) return;          // a {"ok":false} "no data" beat → fall through to idle, keep last numbers
    last_data = *data;
    have_last_data = true;
    last_data_ms = lv_tick_get();   // a real usage update just landed
    data_received = true;

    if (data->clock_epoch > 0) {    // daemon supplied wall-clock time → drive the title clock
        if (clock_fmt != data->clock_fmt) clock_last_min = -1;
        clock_base_epoch = data->clock_epoch;
        clock_base_ms = last_data_ms;
        clock_fmt = data->clock_fmt;
        apply_title_font();
    } else if (clock_base_epoch != 0) {   // clock turned off daemon-side → revert title to "Usage"
        clock_base_epoch = 0;
        clock_last_min = -1;
        lv_label_set_text(lbl_title, "Usage");
        apply_title_font();
    }

    int s_pct = (int)(data->session_pct + 0.5f);
    const bool resets_card = codex_resets_card(data);
    // No 5-hour limit: the one card left sits in the middle of the space the
    // two cards take, unless a Codex account's Resets card takes the second.
    const bool fable = fable_card(data);
    const bool lone = data->weekly_only && !resets_card && !fable;

    // LVGL skips the set when the position hasn't changed.
    if (panel_session)
        lv_obj_set_pos(panel_session, lone ? layout_lone_card_x(L) : L.card[0].x,
                       lone ? layout_lone_card_y(L) : L.card[0].y);

    if (data->enterprise) {
        // Spending box: big number-only label + small "%" symbol + desc + pace
        lv_obj_set_style_text_font(lbl_session_pct, L.ent_pct_font, 0);
        lv_label_set_text(lbl_session_label, L.pills[2]);
        lv_obj_add_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status,   LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
    } else {
        // No 5-hour limit (verified Ultra): the week moves up into the first
        // card and the second card goes away. A Codex account's main meter is
        // "Codex", whichever window it is.
        lv_obj_set_style_text_font(lbl_session_pct, L.pct_font, 0);
        lv_label_set_text(lbl_session_label, L.pills[data->codex ? 4 : data->weekly_only ? 1 : 0]);
        lv_obj_clear_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) {
            if (lone) lv_obj_add_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
            else      lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
        }
    }

    char buf[48];

    if (data->enterprise) {
        lv_label_set_text_fmt(lbl_session_pct, "%d", s_pct);
        lv_obj_align_to(lbl_session_pct_sym, lbl_session_pct,
                        LV_ALIGN_OUT_RIGHT_TOP, 4, 12);
    } else {
        lv_label_set_text_fmt(lbl_session_pct, "%d%%", s_pct);
        format_reset_time(data->session_reset_mins, buf, sizeof(buf), data->weekly_only ? "---" : "Not started");
        lv_label_set_text(lbl_session_reset, buf);
    }

    lv_bar_set_value(bar_session, s_pct, LV_ANIM_ON);

    if (data->enterprise) {
        // Period box: time % + dynamic pace color + "Resets <date>" label
        lv_label_set_text(lbl_weekly_label, L.pills[3]);
        lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", data->time_pct);
        lv_bar_set_value(bar_weekly, data->time_pct, LV_ANIM_ON);
        lv_obj_set_style_text_font(lbl_weekly_reset, L.period_font, 0);
        apply_period_colors(data);
    } else if (resets_card) {
        draw_resets_card(data);
    } else if (fable) {
        const int f_pct = data->fable_pct;
        lv_label_set_text(lbl_weekly_label, "Fable");
        lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", f_pct);
        lv_bar_set_value(bar_weekly, f_pct > 100 ? 100 : f_pct, LV_ANIM_ON);
        format_reset_time(data->session_reset_mins, buf, sizeof(buf), "---");
        lv_obj_set_style_text_font(lbl_weekly_reset, L.reset_font, 0);
        lv_label_set_text(lbl_weekly_reset, buf);
    } else {
        int w_pct = (int)(data->weekly_pct + 0.5f);
        lv_label_set_text(lbl_weekly_label, L.pills[1]);
        lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", w_pct);
        lv_bar_set_value(bar_weekly, w_pct, LV_ANIM_ON);
        format_reset_time(data->weekly_reset_mins, buf, sizeof(buf), "---");
        lv_obj_set_style_text_font(lbl_weekly_reset, L.reset_font, 0);
        lv_label_set_text(lbl_weekly_reset, buf);
    }
    if (!resets_card) hide_resets_card();
    apply_bar_colors(data);
}

// Pick the usage-view sub-screen: pairing hint (BLE down), stale meters
// (connected but data has gone stale: dimmed, last values kept), or the live
// meters. Only restyles on an actual change. The animated status line stays
// visible everywhere and names the state.
static void update_view_state(void) {
    if (!usage_group || !pair_group) return;
    int v;
    if (!s_ble_connected) {
        v = 0;  // pairing hint
    } else if (data_received && data_ok && (lv_tick_get() - last_data_ms) < DATA_FRESH_MS) {
        v = 2;  // live usage
    } else {
        v = 1;  // stale
    }
    if (v == view_state) return;
    view_state = v;
    if (v == 0) {
        lv_obj_add_flag(usage_group, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(usage_group, LV_OBJ_FLAG_HIDDEN);
        // `opa` (not `opa_layered`) scales every draw of the group and its
        // children: no layer buffer, so it costs nothing on the C6.
        // mono_panel: faded text prints broken in 1 bit, so it never fades
        const bool dim = v == 1 && !board_caps().mono_panel;
        lv_obj_set_style_opa(usage_group, dim ? STALE_OPA : LV_OPA_COVER, 0);
    }
}

bool ui_meters_live(void) { return view_state == 2; }

void ui_tick_anim(void) {
    update_view_state();
    if (pair_hint == PAIR_HINT_CLEARED && lv_tick_get() - pair_hint_since >= PAIR_CLEARED_SHOW_MS)
        ui_set_pair_hint(PAIR_HINT_NORMAL);
    const bool still = minimal_mode();   // minimal: nothing moves but the numbers

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

    if (alert_active) return;          // the status line holds the alert message

    if (still) {
        // One glyph, one word per state; redrawn only when it changes.
        const char* text = !s_ble_connected ? "Waiting"
                         : view_state == 1 ? (data_received ? "Stale" : "No data") : "Connected";
        if (text != still_shown) {
            still_shown = text;
            static char sbuf[40];
            snprintf(sbuf, sizeof(sbuf), "%s %s", spinner_frames[1], text);
            lv_label_set_text(lbl_anim, sbuf);
        }
        return;
    }

    // The verb changes every verb_ms; at No-nonsense that's once a minute
    // and the spinner holds still, so the line only redraws when the word
    // does (clawd_policy.h, liveliness).
    const ClawdTuning& tune = clawd_tuning();
    if (now - anim_msg_start >= tune.verb_ms) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_verb_idx = fitting_verb((anim_verb_idx + 1) % ANIM_MSG_COUNT);
        anim_msg_start = now;
    }

    if (tune.spinner) {
        if (now - anim_last_ms < spinner_ms[anim_spinner_idx]) return;
        anim_last_ms = now;
        anim_phase = (anim_phase + 1) % SPINNER_PHASES;
        anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                        : (SPINNER_PHASES - anim_phase);
        frozen_shown = nullptr;
    } else {
        anim_spinner_idx = 1;
    }

    // Status text by priority. Whimsical messages only when connected & settled.
    const char* text;
    if (!s_ble_connected) {
        text = "Waiting";              // advertising / waiting for a host connection
    } else if (view_state == 1) {      // stale — alternate so it reads as alive AND not live
        // A mono panel doesn't dim stale meters, so the word is the only cue: hold it.
        const char* stale = data_received ? "Stale" : "No data";
        text = (board_caps().mono_panel || (anim_msg_idx & 1)) ? stale : "Listening";
    } else if (now - connected_at_ms < 5000) {
        text = "Connected";
    } else {
        text = anim_messages[anim_verb_idx];
    }

    if (!tune.spinner) {
        if (text == frozen_shown) return;
        frozen_shown = text;
    }

    // All states share the whimsical style: "<glyph> <Title-case word>…"
    static char buf[80];
    snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
             spinner_frames[anim_spinner_idx], text);
    lv_label_set_text(lbl_anim, buf);
}

static void apply_battery_visibility(void) {
    if (battery_img) lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
}

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    (void)name; (void)mac;
    bool was_connected = s_ble_connected;
    s_ble_connected = (state == BLE_STATE_CONNECTED);

    if (s_ble_connected && !was_connected) connected_at_ms = lv_tick_get();
    // pair / idle / usage — picked from connection + data freshness.
    update_view_state();
}

void ui_update_battery(int percent, bool charging, bool usb_power) {
    if (!battery_img) return;
    int idx;
    if (percent < 0 && usb_power) {
        idx = 5;   // no battery, running on USB: show the plug-zap glyph, not an empty battery
    } else if (charging) {
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

void ui_set_alert_text(const char* message) {
    if (message && message[0]) {
        alert_active = true;
        // Minimal mode: nothing moves, so a long message is cut, not scrolled.
        lv_label_set_long_mode(lbl_anim, minimal_mode() ? LV_LABEL_LONG_DOT
                                                        : LV_LABEL_LONG_SCROLL_CIRCULAR);
        // One line: without a fixed height LONG_DOT would wrap onto the cards.
        lv_obj_set_size(lbl_anim, L.status_w, lv_font_get_line_height(L.status_font));
        lv_label_set_text(lbl_anim, message);
        return;
    }
    if (!alert_active) return;
    alert_active = false;
    lv_label_set_long_mode(lbl_anim, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(lbl_anim, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    anim_last_ms = 0;          // whimsical line redraws on the next tick
    still_shown = nullptr;
    frozen_shown = nullptr;
}
