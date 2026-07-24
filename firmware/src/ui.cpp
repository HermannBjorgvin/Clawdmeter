#include "ui.h"
#include "splash.h"
#include <lvgl.h>
#include <Arduino.h>
#include <time.h>
#include <math.h>
#include "logo.h"
#include "icons.h"
#include "hal/board_caps.h"
#include "usage_history.h"
#include "flame_icon.h"
#include "costumes.h"
#include <Preferences.h>

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
    } else if (c.height >= 300) {
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

// ---- Corner indicators (top-right): burn-rate flame + battery when present ----
static lv_obj_t* flame_img;             // burn-rate indicator, always shown
static lv_image_dsc_t flame_dsc;        // single flame bitmap; scaled/faded by burn rate
static uint16_t  flame_max_scale = 256; // 256 = 48px; halved on small-icon boards
static lv_obj_t* battery_img = nullptr; // just left of the flame; only when a cell is attached
static lv_image_dsc_t battery_dscs[5];  // empty, low, medium, full, charging
static lv_obj_t* prop_img = nullptr;               // costume overlay on the corner Claude
static lv_image_dsc_t costume_dscs[COSTUME_COUNT]; // [0] unused (none)
static int current_costume = 0;
static lv_obj_t* logo_img;

// Latest session/weekly %, cached for the Burn page's ETA math (set in ui_update).
static float s_cur_session = 0.0f, s_cur_weekly = 0.0f;

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
static uint32_t last_swipe_tick = 0;       // set when a swipe fires; splash stands down briefly
static inline bool click_after_swipe(void) { return (lv_tick_get() - last_swipe_tick) < 500; }

// ---- Systemic swipe detection ------------------------------------------------
// One gesture handler is attached to every page BACKGROUND (containers + splash)
// via attach_swipe(). Because it lives on the background, a press that lands on a
// button is handled by the button and never reaches here — so swipe-vs-tap is
// decided structurally and no button needs any special-casing. The recognised
// direction is applied from the main loop (deferred, so we don't re-enter LVGL
// show/hide during event processing).
static int s_pending_page_dir    = 0;
static int s_pending_costume_dir = 0;
static bool     s_touch_down = false;   // a finger is on the page background right now
static uint32_t s_kiosk_last = 0;       // last auto/manual page change (kiosk dwell timer)
#define KIOSK_DWELL_MS  8000
#define READ_HOLD_MS    400             // press held >= this without a swipe = "reading", no action

static void gesture_event_cb(lv_event_t* e) {
    static int16_t  x0 = 0, y0 = 0;
    static uint32_t t0 = 0;
    lv_indev_t* indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p; lv_indev_get_point(indev, &p);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        x0 = (int16_t)p.x; y0 = (int16_t)p.y; t0 = lv_tick_get();
        return;
    }
    // RELEASED: classify the drag. 55px + a dominant axis keeps a tap that drifts
    // a little from becoming a swipe.
    int dx = (int)p.x - x0, dy = (int)p.y - y0;
    uint32_t dt = lv_tick_get() - t0;
    const int SWIPE_MIN = 55;
    if (dt < 800 && abs(dx) >= SWIPE_MIN && abs(dx) > abs(dy) + 12) {
        s_pending_page_dir = (dx < 0) ? -1 : 1;      // horizontal → pages
        ui_note_swipe();
    } else if (dt < 800 && abs(dy) >= SWIPE_MIN && abs(dy) > abs(dx) + 12) {
        s_pending_costume_dir = (dy < 0) ? -1 : 1;   // down → next costume, up → prev
        ui_note_swipe();
    } else if (dt >= READ_HOLD_MS) {
        ui_note_swipe();   // a press-and-hold to read is not a tap — don't toggle the splash
    }
}

// Attach the background swipe handler to a page container / splash root. The
// object must be clickable for press/release events to fire on it (buttons, being
// clickable children on top, intercept their own presses so this never sees them).
static void attach_swipe(lv_obj_t* obj) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, gesture_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(obj, gesture_event_cb, LV_EVENT_RELEASED, NULL);
}

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

static void init_flame_icon(void) {
    init_icon_dsc_rgb565a8(&flame_dsc, ICON_FLAME_W, ICON_FLAME_H, icon_flame_data);
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

static void init_costumes(void) {
    for (int i = 1; i < COSTUME_COUNT; i++)
        init_icon_dsc_rgb565a8(&costume_dscs[i], costumes[i].w, costumes[i].h, costumes[i].data);
    Preferences p; p.begin("clawdmeter", true);
    current_costume = p.getInt("costume", 0);
    p.end();
    if (current_costume < 0 || current_costume >= COSTUME_COUNT) current_costume = 0;
}

// Show the current costume overlay on the corner Claude (hidden when "none" or
// on the splash, where the corner logo itself is hidden).
static void apply_costume(void) {
    if (!prop_img) return;
    if (current_costume == 0 || current_screen == SCREEN_SPLASH) {
        lv_obj_add_flag(prop_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_image_set_src(prop_img, &costume_dscs[current_costume]);
        lv_obj_set_pos(prop_img, L.margin + costumes[current_costume].ox,
                                 L.logo_y + costumes[current_costume].oy);
        lv_obj_clear_flag(prop_img, LV_OBJ_FLAG_HIDDEN);
    }
}

// Swipe up (+1) / down (-1) cycles Claude's costume; persisted across reboots.
void ui_cycle_costume(int dir) {
    current_costume = (current_costume + dir + COSTUME_COUNT) % COSTUME_COUNT;
    Preferences p; p.begin("clawdmeter", false);
    p.putInt("costume", current_costume);
    p.end();
    apply_costume();
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

    // A shrunk-down sleeping creature (reused claudepix "expression sleep" art)
    // sits between the header and the status line; the animated "Listening…"
    // status line carries the words, so no extra text is needed here.
    lv_obj_t* creature = splash_mini_create(idle_group, "expression sleep", L.idle_px);
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
    attach_swipe(usage_container);

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

// ======== Extra pages: Trend / Burn / Session / Weekly ========
// Each is a full-screen transparent container parented to the base screen
// (created BEFORE logo_img/battery_img so those corner icons stay on top and
// visible on every page). Hidden by default; ui_show_screen() toggles them.
// The corner logo, battery icon and page cycling via PWR are shared chrome.


static lv_obj_t* trend_container = nullptr;
static lv_obj_t* trend_line_session = nullptr;
static lv_obj_t* trend_line_weekly = nullptr;
#define TREND_MAX_POINTS 480   // decimation target: never draw more points than screen pixels
static lv_point_precise_t trend_pts_session[TREND_MAX_POINTS];
static lv_point_precise_t trend_pts_weekly[TREND_MAX_POINTS];
static lv_obj_t* trend_lbl_session = nullptr;
static lv_obj_t* trend_lbl_weekly = nullptr;
static lv_obj_t* trend_lbl_span = nullptr;
static int trend_chart_w = 0, trend_chart_h = 0;

// Trend zoom: the time window shown. − widens the window (zoom out), + narrows it.
static const int trend_windows_min[] = { 15, 60, 240, 720, 1440 };
#define TREND_ZOOM_COUNT ((int)(sizeof(trend_windows_min) / sizeof(trend_windows_min[0])))
static const char* const trend_window_lbls[] = { "15m", "1h", "4h", "12h", "24h" };
static int trend_zoom = 1;   // default index → 1h

static lv_obj_t* burn_container = nullptr;
static lv_obj_t* burn_lbl_session_rate = nullptr;
static lv_obj_t* burn_lbl_session_eta = nullptr;
static lv_obj_t* burn_lbl_weekly_rate = nullptr;
static lv_obj_t* burn_lbl_weekly_eta = nullptr;

static lv_obj_t* system_container = nullptr;
static lv_obj_t* sys_stats_lbl = nullptr;    // multi-line mono stats block
static lv_obj_t* sys_cycle_btn = nullptr;    // kiosk-cycle toggle
static lv_obj_t* sys_cycle_lbl = nullptr;
static bool      s_cycle_mode  = false;      // auto-advance pages (persisted NVS "cycle")

#define RHYTHM_BARS 14
static lv_obj_t* rhythm_container = nullptr;
static lv_obj_t* rhythm_bars[RHYTHM_BARS] = { nullptr };
static int       rhythm_chart_h = 0;
static lv_obj_t* records_container = nullptr;
static lv_obj_t* rec_val[4] = { nullptr, nullptr, nullptr, nullptr };  // 4 big value labels

static lv_obj_t* make_page_container(lv_obj_t* scr) {
    lv_obj_t* c = lv_obj_create(scr);
    lv_obj_set_size(c, L.scr_w, L.scr_h);
    lv_obj_set_pos(c, 0, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    // Tap anywhere still toggles the splash; drag anywhere on the background swipes.
    lv_obj_add_event_cb(c, global_click_cb, LV_EVENT_CLICKED, NULL);
    attach_swipe(c);
    lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    return c;
}

static void make_page_title(lv_obj_t* parent, const char* txt) {
    lv_obj_t* t = lv_label_create(parent);
    lv_label_set_text(t, txt);
    lv_obj_set_style_text_font(t, L.title_font, 0);
    lv_obj_set_style_text_color(t, COL_TEXT, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, L.title_nudge, L.title_y);
}

static void trend_update(void);   // fwd — re-render on zoom change

static void trend_set_zoom(int z) {
    ui_note_swipe();   // insurance: a zoom-button tap must never also toggle the splash
    if (z < 0) z = 0;
    if (z >= TREND_ZOOM_COUNT) z = TREND_ZOOM_COUNT - 1;
    trend_zoom = z;
    Preferences p; p.begin("clawdmeter", false); p.putInt("trendzoom", z); p.end();
    trend_update();
}
static void trend_zoom_out_cb(lv_event_t* e) { (void)e; trend_set_zoom(trend_zoom + 1); }  // − : wider window
static void trend_zoom_in_cb(lv_event_t* e)  { (void)e; trend_set_zoom(trend_zoom - 1); }  // + : narrower window

// Reusable round tap-button (e.g. the Trend zoom −/+). Consumes its own click so
// it never leaks to the splash toggle, and carries a generous invisible touch
// margin (ext_click_area) so it's easy to hit. Lay it out in normal upright
// coordinates — like any widget it works in every orientation for free, because
// touch is rotated at the input layer (see touch_rotate.h / my_touch_cb).
static lv_obj_t* make_round_button(lv_obj_t* parent, const char* txt, lv_event_cb_t cb) {
    // Plain clickable object (not lv_button) so there's no theme press-transition
    // animation — press feedback is an explicit, per-button brighten below.
    lv_obj_t* b = lv_obj_create(parent);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(b, 48, 48);
    lv_obj_set_ext_click_area(b, 22);   // generous touch target beyond the visible circle
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    // Opaque fill: a translucent button appears to "light up" when a zoom
    // re-render moves the chart line beneath it. Outlined so it still reads as a
    // control against the panel; pressed fills with the accent (this button only).
    lv_obj_set_style_bg_color(b, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, COL_ACCENT, 0);
    lv_obj_set_style_border_opa(b, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(b, COL_ACCENT, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, L.pill_font, 0);
    lv_obj_set_style_text_color(l, COL_TEXT, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void build_trend_screen(lv_obj_t* scr) {
    trend_container = make_page_container(scr);
    make_page_title(trend_container, "Trend");

    int panel_h = L.scr_h - L.content_y - 90;
    lv_obj_t* panel = make_panel(trend_container, L.margin, L.content_y, L.content_w, panel_h);
    trend_chart_w = L.content_w - 2 * L.panel_pad_x;
    trend_chart_h = panel_h - 2 * L.panel_pad_y;

    // Weekly drawn first (under), session on top.
    trend_line_weekly = lv_line_create(panel);
    lv_obj_set_style_line_width(trend_line_weekly, 3, 0);
    lv_obj_set_style_line_color(trend_line_weekly, COL_GREEN, 0);
    lv_obj_set_style_line_rounded(trend_line_weekly, true, 0);
    lv_obj_add_flag(trend_line_weekly, LV_OBJ_FLAG_HIDDEN);

    trend_line_session = lv_line_create(panel);
    lv_obj_set_style_line_width(trend_line_session, 3, 0);
    lv_obj_set_style_line_color(trend_line_session, COL_ACCENT, 0);
    lv_obj_set_style_line_rounded(trend_line_session, true, 0);
    lv_obj_add_flag(trend_line_session, LV_OBJ_FLAG_HIDDEN);

    int ly = L.content_y + panel_h + 8;
    trend_lbl_session = lv_label_create(trend_container);
    lv_obj_set_style_text_font(trend_lbl_session, L.reset_font, 0);
    lv_obj_set_style_text_color(trend_lbl_session, COL_ACCENT, 0);
    lv_obj_set_pos(trend_lbl_session, L.margin, ly);
    lv_label_set_text(trend_lbl_session, "Session --%");

    trend_lbl_weekly = lv_label_create(trend_container);
    lv_obj_set_style_text_font(trend_lbl_weekly, L.reset_font, 0);
    lv_obj_set_style_text_color(trend_lbl_weekly, COL_GREEN, 0);
    lv_obj_align(trend_lbl_weekly, LV_ALIGN_TOP_RIGHT, -L.margin, ly);
    lv_label_set_text(trend_lbl_weekly, "Weekly --%");

    trend_lbl_span = lv_label_create(trend_container);
    lv_obj_set_style_text_font(trend_lbl_span, L.pace_font, 0);
    lv_obj_set_style_text_color(trend_lbl_span, COL_DIM, 0);
    lv_obj_align(trend_lbl_span, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_label_set_text(trend_lbl_span, "Gathering data...");

    // Restore the saved zoom, then the −/+ round buttons over the chart corners.
    Preferences pz; pz.begin("clawdmeter", true); trend_zoom = pz.getInt("trendzoom", 1); pz.end();
    if (trend_zoom < 0 || trend_zoom >= TREND_ZOOM_COUNT) trend_zoom = 1;
    lv_obj_t* zb_out = make_round_button(trend_container, "-", trend_zoom_out_cb);  // zoom out
    lv_obj_set_pos(zb_out, L.margin + 6, L.content_y + 6);
    lv_obj_t* zb_in = make_round_button(trend_container, "+", trend_zoom_in_cb);    // zoom in
    lv_obj_set_pos(zb_in, L.scr_w - L.margin - 6 - 48, L.content_y + 6);
}

static lv_obj_t* make_burn_panel(lv_obj_t* parent, int y, int h, const char* name,
                                 lv_color_t accent, lv_obj_t** out_rate, lv_obj_t** out_eta) {
    lv_obj_t* panel = make_panel(parent, L.margin, y, L.content_w, h);

    lv_obj_t* pill = make_pill(panel, name);
    lv_obj_set_style_bg_color(pill, accent, 0);
    lv_obj_align(pill, LV_ALIGN_TOP_LEFT, 0, 0);

    *out_rate = lv_label_create(panel);
    lv_obj_set_style_text_font(*out_rate, L.pct_font, 0);
    lv_obj_set_style_text_color(*out_rate, COL_TEXT, 0);
    lv_obj_set_pos(*out_rate, 0, 48);
    lv_label_set_text(*out_rate, "--");

    *out_eta = lv_label_create(panel);
    lv_obj_set_style_text_font(*out_eta, L.reset_font, 0);
    lv_obj_set_style_text_color(*out_eta, COL_DIM, 0);
    lv_obj_set_pos(*out_eta, 0, 110);
    lv_label_set_text(*out_eta, "Gathering data...");
    return panel;
}

static void build_burn_screen(lv_obj_t* scr) {
    burn_container = make_page_container(scr);
    make_page_title(burn_container, "Burn");

    int ph = (L.scr_h - L.content_y - L.margin - L.usage_panel_gap) / 2;
    make_burn_panel(burn_container, L.content_y, ph, "Session", COL_ACCENT,
                    &burn_lbl_session_rate, &burn_lbl_session_eta);
    make_burn_panel(burn_container, L.content_y + ph + L.usage_panel_gap, ph, "Weekly", COL_GREEN,
                    &burn_lbl_weekly_rate, &burn_lbl_weekly_eta);
}

// ======== System page (device diagnostics + settings) ========

static void fmt_uptime(uint32_t ms, char* buf, size_t n) {
    uint32_t s = ms / 1000, h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
    if (h > 0) snprintf(buf, n, "%luh %02lum", (unsigned long)h, (unsigned long)m);
    else       snprintf(buf, n, "%lum %02lus", (unsigned long)m, (unsigned long)sec);
}

// Live device stats (uptime/heap/temp change constantly) — refreshed ~1/s from
// ui_system_tick() while the page is visible, and once on each data update.
static void system_page_refresh(void) {
    if (!sys_stats_lbl) return;
    char up[24]; fmt_uptime(millis(), up, sizeof(up));
    uint32_t heap_k    = ESP.getFreeHeap() / 1024;
    uint32_t ps_free_k = ESP.getFreePsram() / 1024;
    uint32_t ps_tot_k  = ESP.getPsramSize()  / 1024;
    float tc = temperatureRead();
    int t_whole = (int)tc, t_frac = (int)((tc - (float)t_whole) * 10.0f + 0.5f);
    if (t_frac < 0) t_frac = 0; if (t_frac > 9) t_frac = 9;
    char buf[240];
    snprintf(buf, sizeof(buf),
             "Uptime  %s\n"
             "Heap    %lu KB\n"
             "PSRAM   %lu.%lu / %lu.%lu MB\n"
             "Chip    %d.%d C\n"
             "BLE     %s\n"
             "Build   %s",
             up, (unsigned long)heap_k,
             (unsigned long)(ps_free_k / 1024), (unsigned long)((ps_free_k % 1024) * 10 / 1024),
             (unsigned long)(ps_tot_k / 1024),  (unsigned long)((ps_tot_k % 1024) * 10 / 1024),
             t_whole, t_frac,
             s_ble_connected ? "connected" : "waiting",
             __DATE__);
    lv_label_set_text(sys_stats_lbl, buf);
}

static void system_page_update(const UsageData* data) { (void)data; system_page_refresh(); }

static void sys_cycle_refresh_label(void) {
    if (!sys_cycle_lbl) return;
    lv_label_set_text(sys_cycle_lbl, s_cycle_mode ? "Cycle  ON" : "Cycle  OFF");
    lv_obj_set_style_bg_color(sys_cycle_btn, s_cycle_mode ? COL_GREEN : COL_BAR_BG, 0);
}

static void sys_cycle_toggle_cb(lv_event_t* e) {
    (void)e;
    ui_note_swipe();   // a toggle tap must not also toggle the splash
    s_cycle_mode = !s_cycle_mode;
    Preferences p; p.begin("clawdmeter", false); p.putBool("cycle", s_cycle_mode); p.end();
    sys_cycle_refresh_label();
    // Stay on System (settings) so the toggle stays under your finger; the kiosk
    // begins auto-advancing as soon as you swipe to any page.
}


// ---- System-page settings widgets (shared so every control looks/behaves the same) ----

// A rounded, clickable settings pill with a centered label. Used for both the
// chatter segmented control and the kiosk toggle. Position with lv_obj_set_pos
// after; out_label (nullable) receives the label for later text updates.
static lv_obj_t* make_setting_btn(lv_obj_t* parent, int w, int h, const char* text,
                                  const lv_font_t* font, lv_event_cb_t cb, void* ud,
                                  lv_obj_t** out_label) {
    lv_obj_t* b = lv_obj_create(parent);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(b, w, h);
    lv_obj_set_ext_click_area(b, 16);   // generous margin so taps don't leak to the splash
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_bg_color(b, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t* l = lv_label_create(b);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, COL_TEXT, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    if (out_label) *out_label = l;
    return b;
}

static void build_system_page(lv_obj_t* scr) {
    system_container = make_page_container(scr);
    make_page_title(system_container, "System");
    const bool sm = L.small_icons;

    // --- Device stats panel (height from the font so all 6 rows fit exactly) ---
    const int stat_rows = 6;
    const int line_sp = sm ? 4 : 6;
    const int line_h = lv_font_get_line_height(&font_mono_18);
    const int stats_h = stat_rows * line_h + (stat_rows - 1) * line_sp + 2 * L.panel_pad_y;
    lv_obj_t* panel = make_panel(system_container, L.margin, L.content_y, L.content_w, stats_h);
    sys_stats_lbl = lv_label_create(panel);
    lv_obj_set_style_text_font(sys_stats_lbl, &font_mono_18, 0);
    lv_obj_set_style_text_color(sys_stats_lbl, COL_TEXT, 0);
    lv_obj_set_style_text_line_space(sys_stats_lbl, line_sp, 0);
    lv_obj_set_pos(sys_stats_lbl, 0, 2);
    lv_label_set_text(sys_stats_lbl, "...");

    int y = L.content_y + stats_h + (sm ? 8 : 14);   // running cursor down the page

    // --- Setting: kiosk auto-cycle toggle (accent-outlined full-width pill) ---
    sys_cycle_btn = make_setting_btn(system_container, L.content_w, sm ? 44 : 58, "",
                                     L.pill_font, sys_cycle_toggle_cb, nullptr, &sys_cycle_lbl);
    lv_obj_set_pos(sys_cycle_btn, L.margin, y);
    lv_obj_set_style_border_width(sys_cycle_btn, 2, 0);
    lv_obj_set_style_border_color(sys_cycle_btn, COL_ACCENT, 0);
    lv_obj_set_style_border_opa(sys_cycle_btn, LV_OPA_50, 0);
    sys_cycle_refresh_label();
}

// ======== Rhythm page (last-24h activity: session burn per time bucket) ========

static void build_rhythm_page(lv_obj_t* scr) {
    rhythm_container = make_page_container(scr);
    make_page_title(rhythm_container, "Rhythm");

    int panel_h = L.scr_h - L.content_y - L.margin;
    lv_obj_t* panel = make_panel(rhythm_container, L.margin, L.content_y, L.content_w, panel_h);

    int inner_w = L.content_w - 2 * L.panel_pad_x;
    rhythm_chart_h = panel_h - 2 * L.panel_pad_y;
    int gap = 4;
    int bw = (inner_w - gap * (RHYTHM_BARS - 1)) / RHYTHM_BARS;
    for (int i = 0; i < RHYTHM_BARS; i++) {
        lv_obj_t* b = lv_obj_create(panel);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);   // decorative: let swipes pass through to the page
        lv_obj_set_size(b, bw, 4);
        lv_obj_set_pos(b, i * (bw + gap), rhythm_chart_h - 4);
        lv_obj_set_style_radius(b, 2, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_set_style_bg_color(b, COL_ACCENT, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        rhythm_bars[i] = b;
    }
}

static void rhythm_page_update(const UsageData* data) {
    (void)data;
    if (!rhythm_bars[0]) return;
    float burn[RHYTHM_BARS];
    for (int i = 0; i < RHYTHM_BARS; i++) burn[i] = 0.0f;
    int n = usage_history_count();
    if (n >= 2) {
        uint32_t t0 = usage_history_at(0)->ms;
        uint32_t span = usage_history_at(n - 1)->ms - t0;
        if (span < 1) span = 1;
        float prev = usage_history_at(0)->session;
        for (int i = 1; i < n; i++) {
            const UsageHistPoint* p = usage_history_at(i);
            float d = p->session - prev; prev = p->session;
            if (d <= 0) continue;   // a reset or flat stretch is no burn
            int bucket = (int)((uint64_t)(p->ms - t0) * RHYTHM_BARS / span);
            if (bucket < 0) bucket = 0;
            if (bucket >= RHYTHM_BARS) bucket = RHYTHM_BARS - 1;
            burn[bucket] += d;
        }
    }
    float maxb = 0.001f;
    for (int i = 0; i < RHYTHM_BARS; i++) if (burn[i] > maxb) maxb = burn[i];
    for (int i = 0; i < RHYTHM_BARS; i++) {
        int h = 3 + (int)(burn[i] / maxb * (float)(rhythm_chart_h - 6));
        if (h > rhythm_chart_h) h = rhythm_chart_h;
        lv_obj_set_height(rhythm_bars[i], h);
        lv_obj_set_y(rhythm_bars[i], rhythm_chart_h - h);
    }
}

// ======== Records page (peaks over the stored history) ========

static void build_records_page(lv_obj_t* scr) {
    records_container = make_page_container(scr);
    make_page_title(records_container, "Records");
    int panel_h = L.scr_h - L.content_y - L.margin;
    lv_obj_t* panel = make_panel(records_container, L.margin, L.content_y, L.content_w, panel_h);

    // Four stats spread evenly down the panel: a dim label + a big value each.
    static const char* names[4] = { "Peak session", "Peak weekly", "Peak burn rate", "Resets survived" };
    const lv_font_t* name_font = L.small_icons ? &font_styrene_14 : &font_styrene_20;
    const lv_font_t* val_font  = L.small_icons ? &font_mono_18    : &font_mono_32;
    int content_h = panel_h - 2 * L.panel_pad_y;
    int rowH = content_h / 4;
    int name_h = L.small_icons ? 16 : 24;
    for (int i = 0; i < 4; i++) {
        int top = i * rowH + (rowH - name_h - lv_font_get_line_height(val_font)) / 2;
        if (top < 0) top = i * rowH;
        lv_obj_t* nm = lv_label_create(panel);
        lv_obj_set_style_text_font(nm, name_font, 0);
        lv_obj_set_style_text_color(nm, COL_DIM, 0);
        lv_obj_set_pos(nm, 0, top);
        lv_label_set_text(nm, names[i]);
        rec_val[i] = lv_label_create(panel);
        lv_obj_set_style_text_font(rec_val[i], val_font, 0);
        lv_obj_set_style_text_color(rec_val[i], COL_TEXT, 0);
        lv_obj_set_pos(rec_val[i], 0, top + name_h + 2);
        lv_label_set_text(rec_val[i], "--");
    }
}

static void records_page_update(const UsageData* data) {
    (void)data;
    if (!rec_val[0]) return;
    int n = usage_history_count();
    if (n < 2) {
        for (int i = 0; i < 4; i++) lv_label_set_text(rec_val[i], i == 3 ? "0" : "--");
        return;
    }
    float peak_s = 0, peak_w = 0, peak_burn = 0;
    int resets = 0;
    float prev = usage_history_at(0)->session;
    for (int i = 0; i < n; i++) {
        const UsageHistPoint* p = usage_history_at(i);
        if (p->session > peak_s) peak_s = p->session;
        if (p->weekly  > peak_w) peak_w = p->weekly;
        if (i > 0 && (p->session - prev) < -5.0f) resets++;
        prev = p->session;
    }
    // Peak burn over a ~5-15 min window, so a single noisy sample can't spike it.
    for (int i = 1; i < n; i++) {
        const UsageHistPoint* pi = usage_history_at(i);
        for (int j = i - 1; j >= 0; j--) {
            uint32_t dt = pi->ms - usage_history_at(j)->ms;
            if (dt < 300000UL) continue;   // < 5 min → widen the window
            if (dt > 900000UL) break;      // > 15 min → too wide
            float d = pi->session - usage_history_at(j)->session;
            if (d > 0) {
                float rate = d * 3600000.0f / (float)dt;
                if (rate > peak_burn) peak_burn = rate;
            }
            break;
        }
    }
    int pb_w = (int)peak_burn, pb_f = (int)((peak_burn - (float)pb_w) * 10.0f + 0.5f);
    if (pb_f > 9) { pb_w++; pb_f = 0; }
    lv_label_set_text_fmt(rec_val[0], "%d%%", (int)(peak_s + 0.5f));
    lv_label_set_text_fmt(rec_val[1], "%d%%", (int)(peak_w + 0.5f));
    lv_label_set_text_fmt(rec_val[2], "%d.%d %%/hr", pb_w, pb_f);
    lv_label_set_text_fmt(rec_val[3], "%d", resets);
}

static void trend_update(void) {
    if (!trend_line_session) return;
    int n = usage_history_count();
    if (n >= 1) {
        const UsageHistPoint* last = usage_history_at(n - 1);
        lv_label_set_text_fmt(trend_lbl_session, "Session %d%%", (int)(last->session + 0.5f));
        lv_label_set_text_fmt(trend_lbl_weekly, "Weekly %d%%", (int)(last->weekly + 0.5f));
    }
    // Keep only samples inside the selected zoom window (oldest→newest by ms).
    int start = 0;
    if (n >= 1) {
        uint32_t last_ms = usage_history_at(n - 1)->ms;
        uint32_t win_ms = (uint32_t)trend_windows_min[trend_zoom] * 60000UL;
        for (int i = 0; i < n; i++)
            if (last_ms - usage_history_at(i)->ms <= win_ms) { start = i; break; }
    }
    int m = n - start;   // samples in the window

    if (m < 2 || trend_chart_w < 2 || trend_chart_h < 2) {
        lv_obj_add_flag(trend_line_session, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(trend_line_weekly, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(trend_lbl_span, "Gathering data...  (zoom %s)", trend_window_lbls[trend_zoom]);
        return;
    }

    // Decimate to <= one point per chart column (bucket-average) so a 24h window
    // (up to 1440 samples) still draws cleanly across ~400px. Stash the averaged
    // values first so we can autoscale Y to the window before plotting.
    int cap = (trend_chart_w < TREND_MAX_POINTS) ? trend_chart_w : TREND_MAX_POINTS;
    int out_n = (m < cap) ? m : cap;
    static float val_s[TREND_MAX_POINTS], val_w[TREND_MAX_POINTS];
    float dhi = 0.0f;   // highest value on screen (tracks overage > 100)
    for (int k = 0; k < out_n; k++) {
        int blo = start + (int)((int64_t)k * m / out_n);
        int bhi = start + (int)((int64_t)(k + 1) * m / out_n);
        if (bhi <= blo) bhi = blo + 1;
        float ss = 0, sw = 0; int cnt = 0;
        for (int i = blo; i < bhi && i < n; i++) {
            const UsageHistPoint* p = usage_history_at(i);
            ss += p->session; sw += p->weekly; cnt++;
        }
        if (cnt == 0) cnt = 1;
        ss /= cnt; sw /= cnt;
        val_s[k] = ss; val_w[k] = sw;
        if (ss > dhi) dhi = ss;
        if (sw > dhi) dhi = sw;
    }

    // Absolute 0-100% axis: 0% is always the floor and heights read as real
    // utilization (weekly 50% sits mid-chart). The ceiling only grows past 100 if
    // the API ever reports overage (utilization > 100%), so a value above the
    // limit is never clipped. A tiny headroom keeps a peak off the top edge.
    float axis_top = 100.0f;
    if (dhi > axis_top) axis_top = dhi * 1.05f;

    for (int k = 0; k < out_n; k++) {
        int32_t x = (out_n < 2) ? 0 : (int32_t)((int64_t)k * (trend_chart_w - 1) / (out_n - 1));
        float vs = val_s[k] < 0 ? 0 : val_s[k];
        float vw = val_w[k] < 0 ? 0 : val_w[k];
        trend_pts_session[k].x = x;
        trend_pts_session[k].y = (trend_chart_h - 1) - (int32_t)(vs * (trend_chart_h - 1) / axis_top);
        trend_pts_weekly[k].x = x;
        trend_pts_weekly[k].y = (trend_chart_h - 1) - (int32_t)(vw * (trend_chart_h - 1) / axis_top);
    }
    lv_line_set_points(trend_line_session, trend_pts_session, out_n);
    lv_line_set_points(trend_line_weekly, trend_pts_weekly, out_n);
    lv_obj_clear_flag(trend_line_session, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(trend_line_weekly, LV_OBJ_FLAG_HIDDEN);

    // Span label: the time span shown + the axis range (0-100 normally, wider on
    // overage), so the axis is legible.
    int span_min = (int)((usage_history_at(n - 1)->ms - usage_history_at(start)->ms) / 60000UL);
    char tspan[16];
    if (span_min < 100) snprintf(tspan, sizeof(tspan), "%dm", span_min);
    else                snprintf(tspan, sizeof(tspan), "%dh%02dm", span_min / 60, span_min % 60);
    lv_label_set_text_fmt(trend_lbl_span, "%s   0-%d%%   (zoom %s)",
                          tspan, (int)(axis_top + 0.5f), trend_window_lbls[trend_zoom]);
}

static void fmt_burn_eta(float pct_now, float rate, char* buf, size_t len) {
    if (rate <= 0.05f) { snprintf(buf, len, "holding steady"); return; }
    float rem = 100.0f - pct_now;
    if (rem <= 0.0f) { snprintf(buf, len, "at the limit"); return; }
    int mins = (int)(rem / rate * 60.0f + 0.5f);
    if (mins < 60)        snprintf(buf, len, "~%dm to full", mins);
    else if (mins < 1440) snprintf(buf, len, "~%dh %dm to full", mins / 60, mins % 60);
    else                  snprintf(buf, len, "~%dd %dh to full", mins / 1440, (mins % 1440) / 60);
}

// LVGL's lv_label_set_text_fmt (lv_snprintf) has NO %f support, so format the
// one-decimal rate from integer parts instead — this was the "f %/hr" bug.
static void set_rate_label(lv_obj_t* lbl, float r) {
    if (r < 0.0f) r = 0.0f;
    int whole = (int)r;
    int frac  = (int)((r - (float)whole) * 10.0f + 0.5f);
    if (frac >= 10) { whole++; frac = 0; }
    lv_label_set_text_fmt(lbl, "%d.%d %%/hr", whole, frac);
}

static void burn_update(void) {
    if (!burn_lbl_session_rate) return;
    char b[48];
    float r;
    if (usage_history_session_rate(&r)) {
        set_rate_label(burn_lbl_session_rate, r);
        fmt_burn_eta(s_cur_session, r, b, sizeof(b));
        lv_label_set_text(burn_lbl_session_eta, b);
    } else {
        lv_label_set_text(burn_lbl_session_rate, "--");
        lv_label_set_text(burn_lbl_session_eta, "Gathering data...");
    }
    if (usage_history_weekly_rate(&r)) {
        set_rate_label(burn_lbl_weekly_rate, r);
        fmt_burn_eta(s_cur_weekly, r, b, sizeof(b));
        lv_label_set_text(burn_lbl_weekly_eta, b);
    } else {
        lv_label_set_text(burn_lbl_weekly_rate, "--");
        lv_label_set_text(burn_lbl_weekly_eta, "Gathering data...");
    }
}

// ======== Page registry ========
//
// One row per cyclable page, each a self-contained unit: build() creates its
// container + contents, update() refreshes it from a new UsageData (nullable for
// a static page). Page creation, show/hide, container lookup and the per-update
// refresh all iterate this table — so adding a page is a new screen_t plus one
// PAGES[] row and its build/update functions, with no other code to touch.

// Per-page data refresh. Each touches only its own page's widgets.
static void usage_page_update(const UsageData* data) {
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
    (void)pace_color;

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

// Trend/Burn already re-render from usage_history; adapt to the update signature.
static void trend_page_update(const UsageData* data) { (void)data; trend_update(); }
static void burn_page_update(const UsageData* data)  { (void)data; burn_update();  }

struct PageDef {
    screen_t     id;
    const char*  name;
    lv_obj_t**   container;                 // set by build()
    void (*build)(lv_obj_t* scr);           // create *container + contents
    void (*update)(const UsageData* data);  // per-data refresh (nullable)
};

// Order matches the screen_t page range so navigation stays in sync.
static const PageDef PAGES[] = {
    { SCREEN_USAGE,  "Usage",  &usage_container,  init_usage_screen,  usage_page_update  },
    { SCREEN_TREND,  "Trend",  &trend_container,  build_trend_screen, trend_page_update  },
    { SCREEN_BURN,   "Burn",   &burn_container,   build_burn_screen,  burn_page_update   },
    { SCREEN_RHYTHM, "Rhythm", &rhythm_container, build_rhythm_page,  rhythm_page_update },
    { SCREEN_RECORDS,"Records",&records_container,build_records_page, records_page_update},
    { SCREEN_SYSTEM, "System", &system_container, build_system_page,  system_page_update },
};
static const int PAGE_N = (int)(sizeof(PAGES) / sizeof(PAGES[0]));

// Hide every cyclable page container; ui_show_screen then reveals one.
static void hide_all_pages(void) {
    for (int i = 0; i < PAGE_N; i++)
        if (*PAGES[i].container) lv_obj_add_flag(*PAGES[i].container, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t* page_container(screen_t s) {
    for (int i = 0; i < PAGE_N; i++)
        if (PAGES[i].id == s) return *PAGES[i].container;
    return nullptr;
}

// ======== Public API ========

void ui_init(void) {
    compute_layout(board_caps());
    { Preferences p; if (p.begin("clawdmeter", true)) {
        s_cycle_mode = p.getBool("cycle", false);
        p.end();
    } }

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    if (L.small_icons) init_icon_dsc_rgb565a8(&logo_dsc, LOGO_SMALL_WIDTH, LOGO_SMALL_HEIGHT, logo_small_data);
    else               init_icon_dsc_rgb565a8(&logo_dsc, LOGO_WIDTH, LOGO_HEIGHT, logo_data);
    init_flame_icon();
    init_costumes();

    // Build every page from the registry, before logo/battery so the corner
    // chrome stays on top. Adding a page is a single row in PAGES[].
    for (int i = 0; i < PAGE_N; i++) PAGES[i].build(scr);
    splash_init(scr);

    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
        attach_swipe(splash_get_root());
    }

    logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, &logo_dsc);
    lv_obj_set_pos(logo_img, L.margin, L.logo_y);
    attach_swipe(logo_img);   // keep swipes that start on the corner logo working

    // Costume overlay, on top of the corner Claude. Props are drawn to align
    // with the 80px logo, so only enable them on full-size (non-small) boards.
    if (!L.small_icons) {
        prop_img = lv_image_create(scr);
        lv_obj_set_pos(prop_img, L.margin, L.logo_y);
        lv_obj_add_flag(prop_img, LV_OBJ_FLAG_HIDDEN);
        apply_costume();
    }

    // Corner indicators (top-right): the burn-rate flame at the far right, and
    // the battery just to its left when a cell is actually attached. On small-
    // icon boards the flame is scaled down to match the smaller battery glyph.
    flame_max_scale = L.small_icons ? 128 : 256;
    flame_img = lv_image_create(scr);
    lv_image_set_src(flame_img, &flame_dsc);
    lv_obj_set_pos(flame_img, L.scr_w - ICON_FLAME_W - L.margin, L.batt_y);
    lv_image_set_pivot(flame_img, ICON_FLAME_W / 2, ICON_FLAME_H / 2);
    lv_image_set_scale(flame_img, (uint16_t)(flame_max_scale * 0.66f));  // small idle candle
    lv_obj_set_style_image_opa(flame_img, 150, 0);

    // Battery only on boards with battery hardware; ui_update_battery() reveals
    // it (left of the flame) only while a cell is really attached (pct >= 0).
    if (board_caps().has_battery) {
        init_battery_icons();
        battery_img = lv_image_create(scr);
        lv_image_set_src(battery_img, &battery_dscs[0]);
        lv_obj_set_pos(battery_img, L.scr_w - ICON_FLAME_W - L.margin - 6 - L.batt_w, L.batt_y);
        lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    }
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

    // Feed history (with the daemon's wall-clock for staleness), then refresh
    // every page from the registry (see PAGES[]).
    usage_history_add(data->session_pct, data->weekly_pct, data->clock_epoch);
    s_cur_session = data->session_pct;
    s_cur_weekly  = data->weekly_pct;
    for (int i = 0; i < PAGE_N; i++)
        if (PAGES[i].update) PAGES[i].update(data);
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

void ui_tick_anim(void) {
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
static void apply_flame_visibility(void) {
    if (!flame_img) return;
    if (current_screen == SCREEN_SPLASH) lv_obj_add_flag(flame_img, LV_OBJ_FLAG_HIDDEN);
    else                                  lv_obj_clear_flag(flame_img, LV_OBJ_FLAG_HIDDEN);
}

static bool battery_present = false;   // a cell is actually attached (pct >= 0 or charging)

static void apply_battery_visibility(void) {
    if (!battery_img) return;
    if (battery_present && current_screen != SCREEN_SPLASH)
        lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
}

// Battery indicator (left of the flame) — shown only while a cell is present.
// power_hal reports pct == -1 when none, so a battery-capable board with an
// empty connector hides it instead of drawing a misleading empty glyph.
void ui_update_battery(int percent, bool charging) {
    if (!battery_img) return;
    battery_present = (percent >= 0) || charging;
    if (battery_present) {
        int idx = charging      ? 4 :
                  percent <= 10 ? 0 :
                  percent <= 35 ? 1 :
                  percent <= 75 ? 2 : 3;
        lv_image_set_src(battery_img, &battery_dscs[idx]);
    }
    apply_battery_visibility();
}

// Set (via ui_note_swipe) the instant a swipe is recognised, so the tap handler
// below stands down: with scrolling disabled on our containers, LVGL still emits
// CLICKED after a swipe, which would otherwise toggle the splash mid-swipe.
void ui_note_swipe(void) { last_swipe_tick = lv_tick_get(); }

// Called from the input layer every read with the raw touch state. The kiosk
// treats ANY touch as activity: it holds while a finger is down (see
// ui_kiosk_tick) and the dwell restarts once released — so a button tap, a swipe,
// or just resting a finger on a page all interrupt the auto-cycle.
void ui_note_touch(bool down) { s_touch_down = down; }

static void ui_auto_advance(void);   // fwd (defined below, used by ui_kiosk_tick)

// Apply any swipe recognised by the background gesture handler. Called from the
// main loop so page show/hide never runs inside LVGL event processing.
void ui_apply_pending_gestures(void) {
    if (s_pending_page_dir) {
        int d = s_pending_page_dir; s_pending_page_dir = 0;
        if (d > 0) ui_next_page(); else ui_prev_page();
        s_kiosk_last = lv_tick_get();   // a manual page pick restarts the kiosk dwell
    }
    if (s_pending_costume_dir) {
        int d = s_pending_costume_dir; s_pending_costume_dir = 0;
        ui_cycle_costume(d);
    }
}

// Kiosk auto-cycle tick (call each loop). Advances pages on a timer when Cycle is
// on, but only while on a cyclable page other than System (the settings page), and
// a manual swipe (above) restarts the dwell so it never stomps your selection.
void ui_kiosk_tick(void) {
    if (s_touch_down) { s_kiosk_last = lv_tick_get(); return; }   // finger down (reading) → hold
    if (!s_cycle_mode) return;
    if (current_screen < SCREEN_PAGE_FIRST || current_screen > SCREEN_PAGE_LAST ||
        current_screen == SCREEN_SYSTEM) {
        s_kiosk_last = lv_tick_get();   // paused on splash / settings
        return;
    }
    if (lv_tick_get() - s_kiosk_last >= KIOSK_DWELL_MS) {
        ui_auto_advance();
        s_kiosk_last = lv_tick_get();
    }
}

static void global_click_cb(lv_event_t* e) {
    (void)e;
    if (click_after_swipe()) return;  // this "click" was really a swipe
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

void ui_show_screen(screen_t screen) {
    hide_all_pages();
    splash_hide();

    if (screen == SCREEN_SPLASH) {
        splash_show();
    } else {
        lv_obj_t* c = page_container(screen);
        if (c) lv_obj_clear_flag(c, LV_OBJ_FLAG_HIDDEN);
        else { screen = SCREEN_USAGE; lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN); }
    }

    if (logo_img) {
        if (screen == SCREEN_SPLASH) lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        else                          lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
    }

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
    apply_flame_visibility();
    apply_battery_visibility();
    apply_costume();
}

#define PAGE_COUNT (SCREEN_PAGE_LAST - SCREEN_PAGE_FIRST + 1)

// Step `delta` pages from the current one, wrapping with modulo. From the splash
// (out of the page range) we enter at the first/last page depending on direction.
static void ui_step_page(int delta) {
    screen_t s = current_screen;
    if (s < SCREEN_PAGE_FIRST || s > SCREEN_PAGE_LAST) {
        ui_show_screen(delta >= 0 ? SCREEN_PAGE_FIRST : SCREEN_PAGE_LAST);
        return;
    }
    int idx = (s - SCREEN_PAGE_FIRST + delta) % PAGE_COUNT;
    if (idx < 0) idx += PAGE_COUNT;                    // C's % can go negative
    ui_show_screen((screen_t)(SCREEN_PAGE_FIRST + idx));
}

void ui_next_page(void) { ui_step_page(+1); }
void ui_prev_page(void) { ui_step_page(-1); }

static void ui_auto_advance(void) {
    ui_step_page(+1);
    if (current_screen == SCREEN_SYSTEM) ui_step_page(+1);  // kiosk skips the settings page
}

void ui_system_tick(void) {
    if (current_screen != SCREEN_SYSTEM) return;
    static uint32_t last = 0;
    uint32_t now = lv_tick_get();
    if (now - last < 1000) return;   // uptime/heap/temp are fine at ~1 Hz
    last = now;
    system_page_refresh();
}

void ui_trend_refresh(void) { trend_update(); }
void ui_trend_zoom_step(int dir) {   // wraps, so the dev 'zoom' cmd can cycle every window
    int z = (trend_zoom + dir) % TREND_ZOOM_COUNT;
    if (z < 0) z += TREND_ZOOM_COUNT;
    trend_set_zoom(z);
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

// Burn-rate corner indicator: an animated fire emoji. Called every loop; it
// (1) smoothly tracks the live session burn rate as a base intensity, and
// (2) adds a per-frame organic flicker whose amplitude grows with intensity —
// a gently breathing candle when idle, a wild roaring flame when you're
// hammering Claude. Replaces the old 4-step tier + the battery glyph.
#define BURN_RATE_FULL 20.0f   // %/hr that maps to a full-size flame (~fills 5h session)

static float flame_base = 0.0f;   // smoothed 0..1 base intensity

void ui_flame_tick(void) {
    if (!flame_img || current_screen == SCREEN_SPLASH) return;
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last < 45) return;   // ~22 fps is plenty for a flicker
    last = now;

    // Target intensity from the live session rate (0 until ~4 min of history).
    float rate = 0.0f;
    float target = usage_history_session_rate(&rate) ? (rate / BURN_RATE_FULL) : 0.0f;
    if (target < 0.0f) target = 0.0f;
    if (target > 1.0f) target = 1.0f;
    flame_base += (target - flame_base) * 0.05f;   // ease toward target (~1s)

    // Idle floor so it's always a visible little candle, then organic flicker.
    float inten = 0.22f + 0.78f * flame_base;
    float t = now / 1000.0f;
    float wave = sinf(t * 11.0f) * 0.5f + sinf(t * 17.3f) * 0.3f + sinf(t * 27.1f) * 0.2f;
    inten += (0.06f + 0.16f * flame_base) * wave;   // flicker grows with heat
    if (inten < 0.15f) inten = 0.15f;
    if (inten > 1.0f)  inten = 1.0f;

    lv_image_set_scale(flame_img, (uint16_t)(flame_max_scale * (0.586f + 0.414f * inten)));
    lv_obj_set_style_image_opa(flame_img, (uint8_t)(140 + inten * (255 - 140)), 0);
}
