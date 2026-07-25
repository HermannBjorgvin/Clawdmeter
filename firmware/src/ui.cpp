#include "ui.h"
#include "splash.h"
#include <lvgl.h>
#include <time.h>
#include <Preferences.h>
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

// Every bundled font is an ASCII-only subset (U+0020..U+007E), so separators
// have to be plain ASCII — " - " stands in for the "·" the design calls for.
#define SEP " - "

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

    // Shared chrome
    const lv_font_t* title_font;     // clock (a few glyphs, so it can be big)
    const lv_font_t* name_font;      // "Clawdmeter" fallback — must clear the
                                     // corner logo and the battery icon
    const lv_font_t* anim_font;      // animated status line (overlays only)
    int16_t anim_y;                  // status line offset from bottom
    bool    small_icons;             // 40px logo + 24px battery (vs 80/48) on small screens
    int16_t title_nudge;             // title x-shift balancing the corner logo
    int16_t logo_y;                  // logo top edge
    int16_t batt_y;                  // battery icon top edge
    int16_t batt_w;                  // battery icon width, for position math

    // Panels / bars shared by several modes
    int16_t bar_h;
    int16_t panel_pad_x, panel_pad_y;
    int16_t pill_pad_x, pill_pad_y;

    // CLASSIC mode — the original two big usage panels
    int16_t usage_panel_h;
    int16_t usage_panel_gap;
    int16_t usage_bar_y;
    int16_t usage_reset_y;
    const lv_font_t* pct_font;       // big percentage number
    const lv_font_t* ent_pct_font;   // enterprise spending number
    const lv_font_t* pill_font;      // "Current" / "Weekly" pill
    const lv_font_t* reset_font;     // "Resets in ..." line
    const lv_font_t* pace_font;      // enterprise "Under/On/Over pace" line

    // CLASSIC bottom context strip
    int16_t cs_y, cs_h, cs_name_w, cs_pct_w, cs_bar_h;
    const lv_font_t* cs_font;

    // Slim limit rows (FOCUS full rows, CHATS combined row)
    int16_t row_h;
    int16_t row_gap;                 // vertical gap between the two FOCUS rows
    int16_t row_lbl_w, row_pct_w, row_rst_w;
    int16_t row_bar_h;
    int16_t row_col_gap;
    int16_t row_half_gap;            // gap between the two halves on CHATS
    const lv_font_t* row_lbl_font;
    const lv_font_t* row_pct_font;
    const lv_font_t* row_rst_font;

    // FOCUS context card
    int16_t fc_y, fc_h;
    int16_t fc_pct_y, fc_tok_dy, fc_bar_y, fc_bar_h;
    int16_t fc_dot_d, fc_dot_gap;
    const lv_font_t* fc_name_font;
    const lv_font_t* fc_tag_font;
    const lv_font_t* fc_pct_font;
    const lv_font_t* fc_tok_font;
    const lv_font_t* fc_foot_font;

    // CHATS list
    int16_t ch_list_y;
    int16_t ch_card_h, ch_card_gap;
    int16_t ch_pad_x, ch_pad_y;
    int16_t ch_bar_y, ch_bar_h, ch_meta_y;
    int16_t ch_pct_w;
    uint8_t ch_max_cards;            // how many fit at this size
    const lv_font_t* ch_name_font;
    const lv_font_t* ch_meta_font;

    // RINGS watch face
    int16_t rg_d, rg_w, rg_gap, rg_y;
    int16_t rg_pct_dy, rg_name_dy;
    int16_t rg_leg_y, rg_leg_h;
    int16_t rg_dot_d, rg_leg_gap, rg_leg_item_gap;
    const lv_font_t* rg_pct_font;
    const lv_font_t* rg_name_font;
    const lv_font_t* rg_leg_font;

    // Pairing hint / idle overlay
    int16_t pair_y1, pair_y2, pair_y3;
    int16_t idle_px;                 // sleeping-creature size on the idle screen
    const lv_font_t* pair_hdr_font;
    const lv_font_t* pair_body_font;

    // Empty states
    const lv_font_t* empty_font;
};
static Layout L = {};

// Pick layout values from the active board's pixel dimensions. The shipped
// boards land on the three breakpoints below; new ports inherit the closest
// one — visually OK, may need a polish pass for pixel-perfect alignment but
// never blocks the port from booting.
static void compute_layout(const BoardCaps& c) {
    L.scr_w = c.width;
    L.scr_h = c.height;
    L.margin = 20;
    L.title_y = 30;

    // Values shared by the two large breakpoints; the small branch below
    // overrides them wholesale.
    L.bar_h = 24;
    L.panel_pad_x = 16;
    L.panel_pad_y = 12;
    L.pill_pad_x = 18;
    L.pill_pad_y = 6;
    L.title_font   = &font_tiempos_56;
    L.name_font    = &font_tiempos_34;
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
    L.ch_max_cards = CTX_MAX_CHATS;

    if (c.height >= 460) {
        // Large layout — tuned for 480x480 (AMOLED-2.16), inherited by the
        // 410x502 watch panel.
        L.content_y = 100;
        L.usage_panel_h = 150;
        L.usage_panel_gap = 16;
        L.usage_bar_y = 56;
        L.usage_reset_y = 94;
        L.pair_hdr_font  = &font_styrene_48;
        L.pair_body_font = &font_styrene_28;
        L.empty_font     = &font_styrene_28;

        L.cs_h = 20; L.cs_name_w = 180; L.cs_pct_w = 56; L.cs_bar_h = 8;
        L.cs_font = &font_styrene_16;

        L.row_h = 30; L.row_gap = 14;
        L.row_lbl_w = 32; L.row_pct_w = 58; L.row_rst_w = 96;
        L.row_bar_h = 10; L.row_col_gap = 12; L.row_half_gap = 20;
        L.row_lbl_font = &font_styrene_20;
        L.row_pct_font = &font_styrene_24;
        L.row_rst_font = &font_styrene_20;

        L.fc_pct_y = 50; L.fc_bar_y = 152; L.fc_bar_h = 14;
        L.fc_dot_d = 10; L.fc_dot_gap = 8;
        L.fc_name_font = &font_styrene_24;
        L.fc_tag_font  = &font_styrene_16;
        L.fc_pct_font  = &font_styrene_48;
        L.fc_tok_font  = &font_styrene_24;
        L.fc_foot_font = &font_styrene_20;

        L.ch_card_gap = 10; L.ch_pad_x = 14; L.ch_pad_y = 6;
        L.ch_bar_y = 28; L.ch_bar_h = 8; L.ch_meta_y = 40; L.ch_pct_w = 58;
        L.ch_name_font = &font_styrene_20;
        L.ch_meta_font = &font_styrene_14;

        L.rg_d = 300; L.rg_w = 16; L.rg_gap = 6; L.rg_y = 96;
        L.rg_pct_dy = -18; L.rg_name_dy = 30;
        L.rg_leg_h = 24; L.rg_dot_d = 10; L.rg_leg_gap = 7; L.rg_leg_item_gap = 22;
        L.rg_pct_font  = &font_styrene_48;
        L.rg_name_font = &font_styrene_20;
        L.rg_leg_font  = &font_styrene_20;
    } else if (c.height >= 300) {
        // Compact layout — tuned for 368x448 (AMOLED-1.8).
        L.content_y = 85;
        L.usage_panel_h = 130;
        L.usage_panel_gap = 12;
        L.usage_bar_y = 48;
        L.usage_reset_y = 78;
        L.pair_hdr_font  = &font_styrene_28;
        L.pair_body_font = &font_styrene_20;
        L.empty_font     = &font_styrene_20;

        L.cs_h = 18; L.cs_name_w = 140; L.cs_pct_w = 46; L.cs_bar_h = 7;
        L.cs_font = &font_styrene_14;

        L.row_h = 26; L.row_gap = 12;
        L.row_lbl_w = 28; L.row_pct_w = 50; L.row_rst_w = 80;
        L.row_bar_h = 8; L.row_col_gap = 10; L.row_half_gap = 16;
        L.row_lbl_font = &font_styrene_16;
        L.row_pct_font = &font_styrene_20;
        L.row_rst_font = &font_styrene_16;

        L.fc_pct_y = 42; L.fc_bar_y = 150; L.fc_bar_h = 12;
        L.fc_dot_d = 8; L.fc_dot_gap = 7;
        L.fc_name_font = &font_styrene_20;
        L.fc_tag_font  = &font_styrene_14;
        L.fc_pct_font  = &font_styrene_48;
        L.fc_tok_font  = &font_styrene_20;
        L.fc_foot_font = &font_styrene_16;

        L.ch_card_gap = 8; L.ch_pad_x = 12; L.ch_pad_y = 6;
        L.ch_bar_y = 28; L.ch_bar_h = 7; L.ch_meta_y = 38; L.ch_pct_w = 50;
        L.ch_name_font = &font_styrene_20;
        L.ch_meta_font = &font_styrene_12;

        L.rg_d = 250; L.rg_w = 14; L.rg_gap = 5; L.rg_y = 95;
        L.rg_pct_dy = -16; L.rg_name_dy = 26;
        L.rg_leg_h = 20; L.rg_dot_d = 8; L.rg_leg_gap = 6; L.rg_leg_item_gap = 18;
        L.rg_pct_font  = &font_styrene_48;
        L.rg_name_font = &font_styrene_16;
        L.rg_leg_font  = &font_styrene_16;
    } else {
        // Small layout — tuned for 240x240 (LCD-1.54 and similar square TFTs).
        // Everything shrinks: fonts two steps down, panels ~half height, the
        // corner logo/battery switch to the 40px/24px small assets, and the
        // chat list drops to three cards because four no longer fit legibly.
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
        L.name_font    = &font_styrene_16;
        L.pct_font     = &font_styrene_24;
        L.ent_pct_font = &font_tiempos_34;
        L.pill_font    = &font_styrene_14;
        L.reset_font   = &font_styrene_14;
        L.pace_font    = &font_styrene_12;
        L.anim_font    = &font_mono_18;
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
        L.pair_hdr_font  = &font_styrene_20;
        L.pair_body_font = &font_styrene_14;
        L.empty_font     = &font_styrene_14;

        L.cs_h = 14; L.cs_name_w = 90; L.cs_pct_w = 30; L.cs_bar_h = 5;
        L.cs_font = &font_styrene_12;

        L.row_h = 18; L.row_gap = 6;
        L.row_lbl_w = 20; L.row_pct_w = 34; L.row_rst_w = 54;
        L.row_bar_h = 6; L.row_col_gap = 6; L.row_half_gap = 10;
        L.row_lbl_font = &font_styrene_12;
        L.row_pct_font = &font_styrene_14;
        L.row_rst_font = &font_styrene_12;

        L.fc_pct_y = 22; L.fc_bar_y = 76; L.fc_bar_h = 8;
        L.fc_dot_d = 5; L.fc_dot_gap = 4;
        L.fc_name_font = &font_styrene_14;
        L.fc_tag_font  = &font_styrene_12;
        L.fc_pct_font  = &font_styrene_24;
        L.fc_tok_font  = &font_styrene_14;
        L.fc_foot_font = &font_styrene_12;

        L.ch_max_cards = 3;
        L.ch_card_gap = 6; L.ch_pad_x = 8; L.ch_pad_y = 4;
        L.ch_bar_y = 18; L.ch_bar_h = 5; L.ch_meta_y = 26; L.ch_pct_w = 34;
        L.ch_name_font = &font_styrene_12;
        L.ch_meta_font = &font_styrene_12;

        L.rg_d = 130; L.rg_w = 9; L.rg_gap = 3; L.rg_y = 46;
        L.rg_pct_dy = -10; L.rg_name_dy = 16;
        L.rg_leg_h = 16; L.rg_dot_d = 5; L.rg_leg_gap = 4; L.rg_leg_item_gap = 12;
        L.rg_pct_font  = &font_styrene_24;
        L.rg_name_font = &font_styrene_12;
        L.rg_leg_font  = &font_styrene_12;
    }

    L.content_w = L.scr_w - 2 * L.margin;

    // Derived from the breakpoint values so a taller/shorter panel of the same
    // class (e.g. 410x502 vs 480x480) stretches instead of clipping.
    L.fc_y = L.content_y + 2 * L.row_h + L.row_gap + L.row_gap;
    L.fc_h = L.scr_h - L.margin - L.fc_y;

    L.ch_list_y = L.content_y + L.row_h + L.row_gap + 4;
    L.ch_card_h = (L.scr_h - L.margin - L.ch_list_y
                   - (L.ch_max_cards - 1) * L.ch_card_gap) / L.ch_max_cards;

    L.cs_y = L.scr_h - L.margin - L.cs_h;
    L.rg_leg_y = L.scr_h - L.margin - L.rg_leg_h;
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

// ---- Shared chrome (siblings of every mode container) ----
static lv_obj_t* lbl_title;     // clock, or "Clawdmeter" until the daemon sends time
static lv_obj_t* battery_img;
static lv_obj_t* logo_img;
static lv_image_dsc_t battery_dscs[5];  // empty, low, medium, full, charging
static lv_image_dsc_t logo_dsc;

// Clock fed by the daemon: base epoch (local wall-clock seconds) + the lv_tick at
// which it landed, so the title ticks forward locally between 60s payloads.
static long     clock_base_epoch = 0;
static uint32_t clock_base_ms = 0;
static int      clock_fmt = 24;   // 12 or 24, set from the daemon payload
static int      clock_last_min = -1;   // last rendered minute; avoids redrawing the title every tick

// ---- Mode containers, indexed by screen_t (index 0 / splash stays NULL) ----
static lv_obj_t* mode_root[SCREEN_COUNT] = {};

// ---- A slim "label / bar / pct / countdown" limit row ----
struct LimitRow {
    lv_obj_t* lbl;
    lv_obj_t* bar;
    lv_obj_t* pct;
    lv_obj_t* rst;   // NULL when the row was built without a countdown column
};

// FOCUS mode
static LimitRow  fc_row_s, fc_row_w;
static lv_obj_t* fc_card;
static lv_obj_t* fc_lbl_name;
static lv_obj_t* fc_lbl_tag;
static lv_obj_t* fc_lbl_pct;
static lv_obj_t* fc_lbl_tok;
static lv_obj_t* fc_bar;
static lv_obj_t* fc_lbl_foot;
static lv_obj_t* fc_dots[CTX_MAX_CHATS - 1];
static lv_obj_t* fc_lbl_empty;

// CHATS mode
static LimitRow  ch_row_s, ch_row_w;
struct ChatCard {
    lv_obj_t* card;
    lv_obj_t* name;
    lv_obj_t* pct;
    lv_obj_t* bar;
    lv_obj_t* meta;
};
static ChatCard  ch_cards[CTX_MAX_CHATS];
static lv_obj_t* ch_lbl_empty;

// CLASSIC mode — the original two big usage panels, verbatim
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
static lv_obj_t* lbl_spending_desc = nullptr;    // "of your monthly budget"
static lv_obj_t* lbl_spending_status = nullptr;  // "Under pace" / "On pace" / "Over pace"
// CLASSIC bottom context strip
static lv_obj_t* cs_lbl_name;
static lv_obj_t* cs_bar;
static lv_obj_t* cs_lbl_pct;

// RINGS mode
#define RG_COUNT 3   // 0 = outer (context), 1 = middle (5h/$), 2 = inner (7d/period)
static lv_obj_t* rg_arc[RG_COUNT];
static lv_obj_t* rg_lbl_pct;
static lv_obj_t* rg_lbl_name;
static lv_obj_t* rg_leg_dot[RG_COUNT];
static lv_obj_t* rg_leg_lbl[RG_COUNT];

// ---- Overlays shared by all four modes ----
static lv_obj_t* pair_group;    // pairing hint — shown when disconnected
static lv_obj_t* idle_group;    // "Zzz" screen — connected but data has gone stale
static lv_obj_t* lbl_anim;      // whimsical status line; only visible with an overlay

// ---- Live-data freshness → which content the modes show ----
static uint32_t  last_data_ms = 0;      // lv_tick when the last valid usage update landed
static bool      data_received = false; // any valid update since boot
static int       view_state = -1;       // -1 unknown / 0 pair / 1 idle / 2 live
static screen_t  applied_screen = SCREEN_COUNT;
static const uint32_t DATA_FRESH_MS = 90000;  // usage counts as "live" within this window (daemon sends ~60s)

// ---- Shared ----
static screen_t current_screen = SCREEN_SPLASH;
static screen_t saved_mode = SCREEN_FOCUS;   // mirrored in NVS, restored at boot
static bool     s_ble_connected = false;     // cached BLE connection state
static uint32_t connected_at_ms = 0;         // when we last entered CONNECTED ("Connected" dwell)

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

// ======== Small helpers ========

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

// Countdown without the "Resets in" prefix — for the slim rows, where the row
// label already says which limit it belongs to.
static void format_reset_short(int mins, char* buf, size_t len) {
    if (mins < 0)          snprintf(buf, len, "--");
    else if (mins < 60)    snprintf(buf, len, "%dm", mins);
    else if (mins < 1440)  snprintf(buf, len, "%dh %dm", mins / 60, mins % 60);
    else                   snprintf(buf, len, "%dd %dh", mins / 1440, (mins % 1440) / 60);
}

static void format_tokens(unsigned used_k, unsigned limit_k, char* buf, size_t len) {
    if (limit_k >= 1000) snprintf(buf, len, "%uk / %uM", used_k, limit_k / 1000);
    else                 snprintf(buf, len, "%uk / %uk", used_k, limit_k);
}

static void format_age(unsigned age_min, char* buf, size_t len) {
    if (age_min == 0)     snprintf(buf, len, "now");
    else if (age_min < 60) snprintf(buf, len, "%um ago", age_min);
    else                   snprintf(buf, len, "%uh ago", age_min / 60);
}

// Exact baseline offset between two fonts, so a small caption can sit on the
// same baseline as a big number without eyeballing pixel nudges.
static int16_t baseline_dy(const lv_font_t* big, const lv_font_t* small) {
    int32_t asc_big   = big->line_height - big->base_line;
    int32_t asc_small = small->line_height - small->base_line;
    return (int16_t)(asc_big - asc_small);
}

static int16_t vcenter(int16_t y, int16_t h, const lv_font_t* f) {
    return (int16_t)(y + (h - lv_font_get_line_height(f)) / 2);
}

// Forward decl — defined near ui_show_screen below
static void global_click_cb(lv_event_t* e);

// The header title is either the daemon's clock (few glyphs, set in the big
// display face) or the product name (ten glyphs, which only fits between the
// corner logo and the battery icon at a smaller size). Re-align after every
// change because the label auto-sizes to its content.
static void set_title(const char* text, bool is_clock) {
    if (!lbl_title) return;
    lv_obj_set_style_text_font(lbl_title, is_clock ? L.title_font : L.name_font, 0);
    lv_label_set_text(lbl_title, text);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, L.title_nudge,
                 is_clock ? L.title_y
                          : L.title_y + baseline_dy(L.title_font, L.name_font));
}

// A transparent, full-screen, non-scrolling container. Every mode and overlay
// sits in one of these so it can be shown/hidden as a unit.
static lv_obj_t* make_layer(lv_obj_t* parent, int y, int h) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_set_size(o, L.scr_w, h);
    lv_obj_set_pos(o, 0, y);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

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
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_EVENT_BUBBLE);
    return bar;
}

static lv_obj_t* make_dot(lv_obj_t* parent, int d, lv_color_t col) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, col, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t* make_label(lv_obj_t* parent, const char* text,
                            const lv_font_t* font, lv_color_t col) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, col, 0);
    return lbl;
}

// Fixed-width, right-aligned, single-line — for numeric columns whose content
// changes width (7% vs 100%) without the column moving.
static lv_obj_t* make_right_label(lv_obj_t* parent, const char* text,
                                  const lv_font_t* font, lv_color_t col, int w) {
    lv_obj_t* lbl = make_label(parent, text, font, col);
    lv_obj_set_width(lbl, w);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_RIGHT, 0);
    return lbl;
}

static lv_obj_t* make_pill(lv_obj_t* parent, const char* text) {
    lv_obj_t* lbl = make_label(parent, text, L.pill_font, COL_TEXT);
    lv_obj_set_style_bg_color(lbl, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl, L.pill_pad_x, 0);
    lv_obj_set_style_pad_right(lbl, L.pill_pad_x, 0);
    lv_obj_set_style_pad_top(lbl, L.pill_pad_y, 0);
    lv_obj_set_style_pad_bottom(lbl, L.pill_pad_y, 0);
    return lbl;
}

// [dim label] [bar] [pct] [countdown]. Pass rst_w = 0 to drop the countdown
// column (the CHATS combined row packs two of these side by side).
static void make_limit_row(lv_obj_t* parent, int x, int y, int w, int rst_w, LimitRow* r) {
    const int cols = rst_w ? 3 : 2;
    int bar_w = w - L.row_lbl_w - L.row_pct_w - rst_w - cols * L.row_col_gap;
    if (bar_w < 20) bar_w = 20;
    int cx = x;

    r->lbl = make_label(parent, "5h", L.row_lbl_font, COL_DIM);
    lv_obj_set_width(r->lbl, L.row_lbl_w);
    lv_label_set_long_mode(r->lbl, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_pos(r->lbl, cx, vcenter(y, L.row_h, L.row_lbl_font));
    cx += L.row_lbl_w + L.row_col_gap;

    r->bar = make_bar(parent, cx, y + (L.row_h - L.row_bar_h) / 2, bar_w, L.row_bar_h);
    cx += bar_w + L.row_col_gap;

    r->pct = make_right_label(parent, "--%", L.row_pct_font, COL_TEXT, L.row_pct_w);
    lv_obj_set_pos(r->pct, cx, vcenter(y, L.row_h, L.row_pct_font));
    cx += L.row_pct_w + L.row_col_gap;

    if (rst_w) {
        r->rst = make_right_label(parent, "--", L.row_rst_font, COL_DIM, rst_w);
        lv_obj_set_pos(r->rst, cx, vcenter(y, L.row_h, L.row_rst_font));
    } else {
        r->rst = nullptr;
    }
}

static void set_limit_row(const LimitRow& r, const char* label, int pct,
                          lv_color_t col, const char* reset_text) {
    lv_label_set_text(r.lbl, label);
    lv_label_set_text_fmt(r.pct, "%d%%", pct);
    lv_bar_set_value(r.bar, pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(r.bar, col, LV_PART_INDICATOR);
    if (r.rst && reset_text) lv_label_set_text(r.rst, reset_text);
}

static void init_icon_dsc_rgb565a8(lv_image_dsc_t* dsc, int w, int h, const uint8_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data = data;
    dsc->data_size = w * h * 3;
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

// ======== Mode persistence (same NVS namespace / pattern as brightness.cpp) ========

static void mode_load(void) {
    Preferences prefs;
    prefs.begin("clawdmeter", true);
    uint8_t saved = prefs.getUChar("ui_mode", 0xFF);
    prefs.end();
    if (saved >= SCREEN_MODE_FIRST && saved <= SCREEN_MODE_LAST) saved_mode = (screen_t)saved;
}

static void mode_store(screen_t s) {
    if (s < SCREEN_MODE_FIRST || s > SCREEN_MODE_LAST) return;   // splash is never persisted
    if (s == saved_mode) return;
    saved_mode = s;
    Preferences prefs;
    prefs.begin("clawdmeter", false);
    prefs.putUChar("ui_mode", (uint8_t)s);
    prefs.end();
}

// ======== FOCUS mode ========

static void init_focus_screen(lv_obj_t* scr) {
    lv_obj_t* root = make_layer(scr, 0, L.scr_h);
    mode_root[SCREEN_FOCUS] = root;
    lv_obj_add_event_cb(root, global_click_cb, LV_EVENT_CLICKED, NULL);

    make_limit_row(root, L.margin, L.content_y, L.content_w, L.row_rst_w, &fc_row_s);
    make_limit_row(root, L.margin, L.content_y + L.row_h + L.row_gap,
                   L.content_w, L.row_rst_w, &fc_row_w);

    fc_card = make_panel(root, L.margin, L.fc_y, L.content_w, L.fc_h);
    const int card_w = L.content_w - 2 * L.panel_pad_x;

    fc_lbl_name = make_label(fc_card, "", L.fc_name_font, COL_TEXT);
    lv_obj_set_width(fc_lbl_name, card_w * 2 / 3);
    lv_label_set_long_mode(fc_lbl_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(fc_lbl_name, 0, 0);

    fc_lbl_tag = make_label(fc_card, "context", L.fc_tag_font, COL_DIM);
    lv_obj_align(fc_lbl_tag, LV_ALIGN_TOP_RIGHT, 0,
                 baseline_dy(L.fc_name_font, L.fc_tag_font));

    fc_lbl_pct = make_label(fc_card, "--%", L.fc_pct_font, COL_TEXT);
    lv_obj_set_pos(fc_lbl_pct, 0, L.fc_pct_y);

    L.fc_tok_dy = baseline_dy(L.fc_pct_font, L.fc_tok_font);
    fc_lbl_tok = make_label(fc_card, "", L.fc_tok_font, COL_DIM);
    lv_obj_align(fc_lbl_tok, LV_ALIGN_TOP_RIGHT, 0, L.fc_pct_y + L.fc_tok_dy);

    fc_bar = make_bar(fc_card, 0, L.fc_bar_y, card_w, L.fc_bar_h);

    // Footer: one dot per background chat, then the roll-up text. Bottom-anchored
    // so a taller panel just gets more breathing room above it.
    for (int i = 0; i < CTX_MAX_CHATS - 1; i++) {
        fc_dots[i] = make_dot(fc_card, L.fc_dot_d, COL_BAR_BG);
        lv_obj_align(fc_dots[i], LV_ALIGN_BOTTOM_LEFT,
                     i * (L.fc_dot_d + L.fc_dot_gap),
                     -(lv_font_get_line_height(L.fc_foot_font) - L.fc_dot_d) / 2);
        lv_obj_add_flag(fc_dots[i], LV_OBJ_FLAG_HIDDEN);
    }
    fc_lbl_foot = make_label(fc_card, "", L.fc_foot_font, COL_DIM);
    lv_obj_align(fc_lbl_foot, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_flag(fc_lbl_foot, LV_OBJ_FLAG_HIDDEN);

    fc_lbl_empty = make_label(fc_card, "No active chats", L.empty_font, COL_DIM);
    lv_obj_align(fc_lbl_empty, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(fc_lbl_empty, LV_OBJ_FLAG_HIDDEN);
}

static void render_focus(const UsageData* d) {
    char buf[48];

    if (d->enterprise) {
        // "$" spending against "per" (period elapsed). No 5h/7d countdowns
        // exist for these accounts, so the second row shows the reset date.
        set_limit_row(fc_row_s, "$", (int)(d->session_pct + 0.5f),
                      pct_color(d->session_pct), "");
        set_limit_row(fc_row_w, "per", d->time_pct, COL_ACCENT, d->reset_date);
    } else {
        format_reset_short(d->session_reset_mins, buf, sizeof(buf));
        set_limit_row(fc_row_s, "5h", (int)(d->session_pct + 0.5f),
                      pct_color(d->session_pct), buf);
        format_reset_short(d->weekly_reset_mins, buf, sizeof(buf));
        set_limit_row(fc_row_w, "7d", (int)(d->weekly_pct + 0.5f),
                      pct_color(d->weekly_pct), buf);
    }

    const bool have = d->chat_count > 0;
    lv_obj_t* live[] = { fc_lbl_name, fc_lbl_tag, fc_lbl_pct, fc_lbl_tok, fc_bar };
    for (lv_obj_t* o : live) {
        if (have) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
    if (have) lv_obj_add_flag(fc_lbl_empty, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_clear_flag(fc_lbl_empty, LV_OBJ_FLAG_HIDDEN);

    int bg = 0, max_bg = 0;
    for (int i = 1; i < d->chat_count && i < CTX_MAX_CHATS; i++) {
        if (bg < CTX_MAX_CHATS - 1) {
            lv_obj_clear_flag(fc_dots[bg], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(fc_dots[bg], pct_color(d->chats[i].pct), 0);
            bg++;
        }
        if (d->chats[i].pct > max_bg) max_bg = d->chats[i].pct;
    }
    for (int i = bg; i < CTX_MAX_CHATS - 1; i++) lv_obj_add_flag(fc_dots[i], LV_OBJ_FLAG_HIDDEN);

    if (bg > 0) {
        snprintf(buf, sizeof(buf), "+%d chat%s" SEP "max %d%%", bg, bg == 1 ? "" : "s", max_bg);
        lv_label_set_text(fc_lbl_foot, buf);
        lv_obj_set_x(fc_lbl_foot, bg * (L.fc_dot_d + L.fc_dot_gap) + L.fc_dot_gap);
        lv_obj_clear_flag(fc_lbl_foot, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(fc_lbl_foot, LV_OBJ_FLAG_HIDDEN);
    }

    if (!have) return;
    const ChatCtx& c = d->chats[0];
    lv_label_set_text(fc_lbl_name, c.name);
    lv_label_set_text_fmt(fc_lbl_pct, "%d%%", (int)c.pct);
    format_tokens(c.used_k, c.limit_k, buf, sizeof(buf));
    lv_label_set_text(fc_lbl_tok, buf);
    lv_bar_set_value(fc_bar, c.pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(fc_bar, pct_color(c.pct), LV_PART_INDICATOR);
}

// ======== CHATS mode ========

static void init_chats_screen(lv_obj_t* scr) {
    lv_obj_t* root = make_layer(scr, 0, L.scr_h);
    mode_root[SCREEN_CHATS] = root;
    lv_obj_add_event_cb(root, global_click_cb, LV_EVENT_CLICKED, NULL);

    // One combined row: 5h on the left half, 7d on the right, no countdowns —
    // the chat cards below are what this mode is for.
    const int half = (L.content_w - L.row_half_gap) / 2;
    make_limit_row(root, L.margin, L.content_y, half, 0, &ch_row_s);
    make_limit_row(root, L.margin + half + L.row_half_gap, L.content_y, half, 0, &ch_row_w);

    for (int i = 0; i < CTX_MAX_CHATS; i++) {
        ChatCard& cc = ch_cards[i];
        if (i >= L.ch_max_cards) { cc.card = nullptr; continue; }

        int y = L.ch_list_y + i * (L.ch_card_h + L.ch_card_gap);
        cc.card = make_panel(root, L.margin, y, L.content_w, L.ch_card_h);
        lv_obj_set_style_pad_left(cc.card, L.ch_pad_x, 0);
        lv_obj_set_style_pad_right(cc.card, L.ch_pad_x, 0);
        lv_obj_set_style_pad_top(cc.card, L.ch_pad_y, 0);
        lv_obj_set_style_pad_bottom(cc.card, L.ch_pad_y, 0);
        if (i == 0) {
            // The current chat gets an accent rail. LVGL draws a single-side
            // border without touching the corner radius, so the card keeps its
            // rounded silhouette.
            lv_obj_set_style_border_side(cc.card, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_width(cc.card, 3, 0);
            lv_obj_set_style_border_color(cc.card, COL_ACCENT, 0);
            lv_obj_set_style_border_opa(cc.card, LV_OPA_COVER, 0);
        }

        const int inner = L.content_w - 2 * L.ch_pad_x - (i == 0 ? 3 : 0);

        cc.name = make_label(cc.card, "", L.ch_name_font, COL_TEXT);
        lv_obj_set_width(cc.name, inner - L.ch_pct_w - 8);
        lv_label_set_long_mode(cc.name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_pos(cc.name, 0, 0);

        cc.pct = make_right_label(cc.card, "", L.ch_name_font, COL_TEXT, L.ch_pct_w);
        lv_obj_align(cc.pct, LV_ALIGN_TOP_RIGHT, 0, 0);

        cc.bar = make_bar(cc.card, 0, L.ch_bar_y, inner, L.ch_bar_h);

        cc.meta = make_label(cc.card, "", L.ch_meta_font, COL_DIM);
        lv_obj_set_width(cc.meta, inner);
        lv_label_set_long_mode(cc.meta, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_pos(cc.meta, 0, L.ch_meta_y);

        lv_obj_add_flag(cc.card, LV_OBJ_FLAG_HIDDEN);
    }

    ch_lbl_empty = make_label(root, "No active chats", L.empty_font, COL_DIM);
    lv_obj_align(ch_lbl_empty, LV_ALIGN_TOP_MID, 0,
                 (L.ch_list_y + L.scr_h - L.margin) / 2
                 - lv_font_get_line_height(L.empty_font) / 2);
}

static void render_chats(const UsageData* d) {
    char buf[64], age[16], tok[32];

    if (d->enterprise) {
        set_limit_row(ch_row_s, "$", (int)(d->session_pct + 0.5f), pct_color(d->session_pct), nullptr);
        set_limit_row(ch_row_w, "per", d->time_pct, COL_ACCENT, nullptr);
    } else {
        set_limit_row(ch_row_s, "5h", (int)(d->session_pct + 0.5f), pct_color(d->session_pct), nullptr);
        set_limit_row(ch_row_w, "7d", (int)(d->weekly_pct + 0.5f), pct_color(d->weekly_pct), nullptr);
    }

    int shown = d->chat_count;
    if (shown > L.ch_max_cards) shown = L.ch_max_cards;

    for (int i = 0; i < CTX_MAX_CHATS; i++) {
        ChatCard& cc = ch_cards[i];
        if (!cc.card) continue;
        if (i >= shown) { lv_obj_add_flag(cc.card, LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_clear_flag(cc.card, LV_OBJ_FLAG_HIDDEN);

        const ChatCtx& c = d->chats[i];
        lv_label_set_text(cc.name, c.name);
        lv_label_set_text_fmt(cc.pct, "%d%%", (int)c.pct);
        lv_bar_set_value(cc.bar, c.pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(cc.bar, pct_color(c.pct), LV_PART_INDICATOR);
        format_tokens(c.used_k, c.limit_k, tok, sizeof(tok));
        format_age(c.age_min, age, sizeof(age));
        snprintf(buf, sizeof(buf), "%s" SEP "%s", tok, age);
        lv_label_set_text(cc.meta, buf);
    }

    if (shown == 0) lv_obj_clear_flag(ch_lbl_empty, LV_OBJ_FLAG_HIDDEN);
    else            lv_obj_add_flag(ch_lbl_empty, LV_OBJ_FLAG_HIDDEN);
}

// ======== CLASSIC mode (the original usage screen) ========

static lv_obj_t* make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                                  lv_obj_t** out_pct, lv_obj_t** out_pill,
                                  lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, L.margin, y, L.content_w, L.usage_panel_h);

    *out_pct = make_label(panel, "---%", L.pct_font, COL_TEXT);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, L.usage_bar_y,
                        L.content_w - 2 * L.panel_pad_x, L.bar_h);

    *out_reset = make_label(panel, "---", L.reset_font, COL_DIM);
    lv_obj_set_pos(*out_reset, 0, L.usage_reset_y);

    return panel;
}

static void init_classic_screen(lv_obj_t* scr) {
    lv_obj_t* root = make_layer(scr, 0, L.scr_h);
    mode_root[SCREEN_CLASSIC] = root;
    lv_obj_add_event_cb(root, global_click_cb, LV_EVENT_CLICKED, NULL);

    panel_session = make_usage_panel(root, L.content_y, "Current",
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset);

    // Enterprise-only overlays inside panel_session — hidden until enterprise data arrives
    lbl_session_pct_sym = make_label(panel_session, "%", L.reset_font, COL_TEXT);
    lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_desc = make_label(panel_session, "of your monthly budget", L.reset_font, COL_DIM);
    lv_obj_set_pos(lbl_spending_desc, 0, L.usage_reset_y);
    lv_obj_add_flag(lbl_spending_desc, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_status = make_label(panel_session, "", L.pace_font, COL_TEXT);
    lv_obj_set_pos(lbl_spending_status, 0, L.usage_reset_y + 20);
    lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);

    panel_weekly = make_usage_panel(root,
                     L.content_y + L.usage_panel_h + L.usage_panel_gap, "Weekly",
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset);
    // Recolor enabled so enterprise period box can color pace and reset separately
    lv_label_set_recolor(lbl_weekly_reset, true);

    // Slim context strip in the strip of space the status line used to occupy —
    // the current chat's context window is visible in every mode.
    cs_lbl_name = make_label(root, "", L.cs_font, COL_DIM);
    lv_obj_set_width(cs_lbl_name, L.cs_name_w);
    lv_label_set_long_mode(cs_lbl_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(cs_lbl_name, L.margin, vcenter(L.cs_y, L.cs_h, L.cs_font));

    int cs_bar_w = L.content_w - L.cs_name_w - L.cs_pct_w - 2 * L.row_col_gap;
    if (cs_bar_w < 20) cs_bar_w = 20;
    cs_bar = make_bar(root, L.margin + L.cs_name_w + L.row_col_gap,
                      L.cs_y + (L.cs_h - L.cs_bar_h) / 2, cs_bar_w, L.cs_bar_h);

    cs_lbl_pct = make_right_label(root, "", L.cs_font, COL_TEXT, L.cs_pct_w);
    lv_obj_set_pos(cs_lbl_pct, L.scr_w - L.margin - L.cs_pct_w,
                   vcenter(L.cs_y, L.cs_h, L.cs_font));
}

static void render_classic(const UsageData* data) {
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

    char buf[64];

    // Pace vars used in both enterprise blocks below
    const char* pace_text = "Under pace";
    const char* pace_hex   = "788c5d";   // matches THEME_GREEN
    if (data->session_pct > (float)data->time_pct + 15.0f) {
        pace_text = "Over pace";  pace_hex = "c0392b";
    } else if (data->session_pct > (float)data->time_pct - 15.0f) {
        pace_text = "On pace";    pace_hex = "d97757";
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

    // Bottom context strip
    if (data->chat_count > 0) {
        const ChatCtx& c = data->chats[0];
        lv_label_set_text(cs_lbl_name, c.name);
        lv_label_set_text_fmt(cs_lbl_pct, "%d%%", (int)c.pct);
        lv_bar_set_value(cs_bar, c.pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(cs_bar, pct_color(c.pct), LV_PART_INDICATOR);
        lv_obj_clear_flag(cs_lbl_name, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(cs_lbl_pct, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(cs_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(cs_lbl_name, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(cs_lbl_pct, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(cs_bar, LV_OBJ_FLAG_HIDDEN);
    }
}

// ======== RINGS mode ========

// Centre the legend row (dot + word, three times) as a block. Called whenever
// the wording changes, which in practice is only pro <-> enterprise.
static void rings_layout_legend(void) {
    lv_obj_update_layout(mode_root[SCREEN_RINGS]);
    int total = 0;
    for (int i = 0; i < RG_COUNT; i++)
        total += L.rg_dot_d + L.rg_leg_gap + lv_obj_get_width(rg_leg_lbl[i]);
    total += (RG_COUNT - 1) * L.rg_leg_item_gap;

    int x = (L.scr_w - total) / 2;
    for (int i = 0; i < RG_COUNT; i++) {
        lv_obj_set_pos(rg_leg_dot[i], x,
                       L.rg_leg_y + (L.rg_leg_h - L.rg_dot_d) / 2);
        x += L.rg_dot_d + L.rg_leg_gap;
        lv_obj_set_pos(rg_leg_lbl[i], x, vcenter(L.rg_leg_y, L.rg_leg_h, L.rg_leg_font));
        x += lv_obj_get_width(rg_leg_lbl[i]) + L.rg_leg_item_gap;
    }
}

static void init_rings_screen(lv_obj_t* scr) {
    lv_obj_t* root = make_layer(scr, 0, L.scr_h);
    mode_root[SCREEN_RINGS] = root;
    lv_obj_add_event_cb(root, global_click_cb, LV_EVENT_CLICKED, NULL);

    const lv_color_t ring_col[RG_COUNT] = { COL_GREEN, COL_ACCENT, COL_GREEN };
    for (int i = 0; i < RG_COUNT; i++) {
        int d = L.rg_d - i * 2 * (L.rg_w + L.rg_gap);
        lv_obj_t* a = lv_arc_create(root);
        rg_arc[i] = a;
        lv_obj_set_size(a, d, d);
        lv_obj_align(a, LV_ALIGN_TOP_MID, 0, L.rg_y + i * (L.rg_w + L.rg_gap));
        // Full circle, 12 o'clock start, clockwise sweep.
        lv_arc_set_rotation(a, 270);
        lv_arc_set_bg_angles(a, 0, 360);
        lv_arc_set_range(a, 0, 100);
        lv_arc_set_value(a, 0);
        lv_obj_remove_style(a, NULL, LV_PART_KNOB);
        lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(a, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_style_arc_width(a, L.rg_w, LV_PART_MAIN);
        lv_obj_set_style_arc_width(a, L.rg_w, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(a, COL_BAR_BG, LV_PART_MAIN);
        lv_obj_set_style_arc_color(a, ring_col[i], LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
    }

    rg_lbl_pct = make_label(root, "-", L.rg_pct_font, COL_TEXT);
    rg_lbl_name = make_label(root, "", L.rg_name_font, COL_DIM);
    // Clamp to the hole the three rings leave in the middle, so a long chat
    // name gets ellipsised instead of running out over the arcs.
    lv_obj_set_width(rg_lbl_name, L.rg_d - 6 * L.rg_w - 4 * L.rg_gap - 8);
    lv_label_set_long_mode(rg_lbl_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(rg_lbl_name, LV_TEXT_ALIGN_CENTER, 0);

    static const char* const leg_text[RG_COUNT] = { "ctx", "5h", "7d" };
    for (int i = 0; i < RG_COUNT; i++) {
        rg_leg_dot[i] = make_dot(root, L.rg_dot_d, ring_col[i]);
        rg_leg_lbl[i] = make_label(root, leg_text[i], L.rg_leg_font, COL_DIM);
    }
    rings_layout_legend();
}

static void render_rings(const UsageData* d) {
    const bool have = d->chat_count > 0;
    int ctx_pct = have ? d->chats[0].pct : 0;

    lv_arc_set_value(rg_arc[0], ctx_pct);
    lv_obj_set_style_arc_color(rg_arc[0], pct_color((float)ctx_pct), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(rg_leg_dot[0], pct_color((float)ctx_pct), 0);

    lv_arc_set_value(rg_arc[1], (int)(d->session_pct + 0.5f));
    lv_arc_set_value(rg_arc[2], d->enterprise ? d->time_pct : (int)(d->weekly_pct + 0.5f));

    static bool leg_ent = false;
    static bool leg_set = false;
    if (!leg_set || leg_ent != d->enterprise) {
        leg_set = true;
        leg_ent = d->enterprise;
        lv_label_set_text(rg_leg_lbl[1], d->enterprise ? "$" : "5h");
        lv_label_set_text(rg_leg_lbl[2], d->enterprise ? "per" : "7d");
        rings_layout_legend();
    }

    if (have) lv_label_set_text_fmt(rg_lbl_pct, "%d%%", ctx_pct);
    else      lv_label_set_text(rg_lbl_pct, "-");
    lv_label_set_text(rg_lbl_name, have ? d->chats[0].name : "");

    // The centre text sits on the rings' shared centre; re-align because the
    // percent label's width changes with the value.
    lv_obj_align_to(rg_lbl_pct, rg_arc[0], LV_ALIGN_CENTER, 0, L.rg_pct_dy);
    lv_obj_align_to(rg_lbl_name, rg_arc[0], LV_ALIGN_CENTER, 0, L.rg_name_dy);
}

// ======== Overlays: pairing hint + idle "Zzz" ========

// Pairing hint — shown when disconnected so the screen isn't empty and the
// user knows how to (re)pair. Wording matches the 3-second release gesture.
static void build_pair_group(lv_obj_t* parent) {
    pair_group = make_layer(parent, L.content_y, L.scr_h - L.content_y);
    lv_obj_add_event_cb(pair_group, global_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* l1 = make_label(pair_group, "To pair", L.pair_hdr_font, COL_TEXT);
    lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, L.pair_y1);

    lv_obj_t* l2 = make_label(pair_group, "hold the power button", L.pair_body_font, COL_DIM);
    lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, L.pair_y2);

    lv_obj_t* l3 = make_label(pair_group, "for 3 seconds, then release", L.pair_body_font, COL_DIM);
    lv_obj_align(l3, LV_ALIGN_TOP_MID, 0, L.pair_y3);

    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
}

// Idle "Zzz" screen — shown when the host is connected but no usage update has
// landed recently (token expired, daemon down, host asleep…). Replaces the
// active mode's content so we never render hours-old numbers as if live.
static void build_idle_group(lv_obj_t* parent) {
    idle_group = make_layer(parent, L.content_y, L.scr_h - L.content_y);
    lv_obj_add_event_cb(idle_group, global_click_cb, LV_EVENT_CLICKED, NULL);

    // A shrunk-down sleeping creature (reused claudepix "expression sleep" art)
    // sits between the header and the status line; the animated "Listening…"
    // status line carries the words, so no extra text is needed here.
    lv_obj_t* creature = splash_mini_create(idle_group, "expression sleep", L.idle_px);
    if (creature) lv_obj_align(creature, LV_ALIGN_CENTER, 0, -20);

    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);
}

// ======== View state ========

// Pick what fills the area under the shared header: the active mode's content,
// the pairing hint (BLE down) or the idle "Zzz" screen (connected but the data
// has gone stale). The whimsical status line rides along with the two overlays
// only — with live numbers on screen it was just noise.
static void apply_view(void) {
    for (int s = SCREEN_MODE_FIRST; s <= SCREEN_MODE_LAST; s++)
        if (mode_root[s]) lv_obj_add_flag(mode_root[s], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_anim, LV_OBJ_FLAG_HIDDEN);

    if (current_screen == SCREEN_SPLASH) return;

    if (view_state == 2) {
        lv_obj_clear_flag(mode_root[current_screen], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(view_state == 0 ? pair_group : idle_group, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_anim, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_view(bool force) {
    if (!pair_group || !idle_group || !lbl_anim) return;
    int v;
    if (!s_ble_connected) {
        v = 0;  // pairing hint
    } else if (data_received && (lv_tick_get() - last_data_ms) < DATA_FRESH_MS) {
        v = 2;  // live data
    } else {
        v = 1;  // idle / Zzz
    }
    if (!force && v == view_state && current_screen == applied_screen) return;
    view_state = v;
    applied_screen = current_screen;
    apply_view();
}

// ======== Public API ========

void ui_init(void) {
    compute_layout(board_caps());
    mode_load();

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    if (L.small_icons) init_icon_dsc_rgb565a8(&logo_dsc, LOGO_SMALL_WIDTH, LOGO_SMALL_HEIGHT, logo_small_data);
    else               init_icon_dsc_rgb565a8(&logo_dsc, LOGO_WIDTH, LOGO_HEIGHT, logo_data);
    init_battery_icons();

    init_focus_screen(scr);
    init_chats_screen(scr);
    init_classic_screen(scr);
    init_rings_screen(scr);
    splash_init(scr);

    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    // Created after the splash so the overlays and the shared header draw on
    // top of it; all of them are hidden while the splash is up.
    build_pair_group(scr);
    build_idle_group(scr);

    lbl_anim = make_label(scr, "", L.anim_font, COL_ACCENT);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, L.anim_y);

    // Shared header: title/clock centred, logo left, battery right — present on
    // every mode and on both overlays.
    // The nudge inside set_title() balances the corner logo on the left;
    // smaller on small screens where the logo is 40px and the battery icon
    // sits closer.
    lbl_title = make_label(scr, "", L.name_font, COL_TEXT);
    set_title("Clawdmeter", false);

    logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, &logo_dsc);
    lv_obj_set_pos(logo_img, L.margin, L.logo_y);

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
    last_data_ms = lv_tick_get();   // a valid usage update just landed
    data_received = true;

    if (data->clock_epoch > 0) {    // daemon supplied wall-clock time → drive the title clock
        clock_base_epoch = data->clock_epoch;
        clock_base_ms = last_data_ms;
        clock_fmt = data->clock_fmt;
    } else if (clock_base_epoch != 0) {   // clock turned off daemon-side → revert to the name
        clock_base_epoch = 0;
        clock_last_min = -1;
        set_title("Clawdmeter", false);
    }

    // Every mode is rendered on every payload — they're all just label/bar
    // writes, and keeping them current means switching modes is instant.
    render_focus(data);
    render_chats(data);
    render_classic(data);
    render_rings(data);

    // First real data ends the boot splash automatically; no button needed.
    if (current_screen == SCREEN_SPLASH) ui_show_screen(saved_mode);
    else                                 refresh_view(false);
}

void ui_tick_anim(void) {
    if (current_screen == SCREEN_SPLASH) return;
    refresh_view(false);
    if (view_state == 1) splash_mini_tick();   // animate the sleeping creature on the idle screen

    uint32_t now = lv_tick_get();

    // Title clock: once the daemon has sent wall-clock time, replace the name
    // with the live time, advanced locally so it ticks every minute between
    // payloads. Shared header → this works in every mode.
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
            set_title(tbuf, true);
        }
    }

    if (view_state == 2) return;   // status line is hidden alongside live data

    if (now - anim_msg_start >= ANIM_MSG_MS) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_msg_start = now;
    }

    if (now - anim_last_ms < spinner_ms[anim_spinner_idx]) return;
    anim_last_ms = now;
    anim_phase = (anim_phase + 1) % SPINNER_PHASES;
    anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                    : (SPINNER_PHASES - anim_phase);

    const char* text;
    if (!s_ble_connected) {
        text = "Waiting";              // advertising / waiting for a host connection
    } else if (now - connected_at_ms < 5000) {
        text = "Connected";
    } else {                           // idle — alternate so it reads as alive AND data-less
        text = (anim_msg_idx & 1) ? "No data" : "Listening";
    }

    static char buf[80];
    snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
             spinner_frames[anim_spinner_idx], text);
    lv_label_set_text(lbl_anim, buf);
}

static void apply_battery_visibility(void) {
    if (!battery_img) return;
    if (current_screen == SCREEN_SPLASH) lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    else                                 lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
}

// A tap anywhere advances the mode ring. From the splash (boot screen only) it
// restores the remembered mode instead — the splash is never re-entered by tap.
static void global_click_cb(lv_event_t* e) {
    (void)e;
    ui_mode_next();
}

static screen_t mode_step(screen_t from, int dir) {
    const int n = SCREEN_MODE_LAST - SCREEN_MODE_FIRST + 1;
    int i = (int)from - SCREEN_MODE_FIRST;
    i = (i + dir + n) % n;
    return (screen_t)(SCREEN_MODE_FIRST + i);
}

void ui_mode_next(void) {
    if (current_screen == SCREEN_SPLASH) ui_show_screen(saved_mode);
    else                                 ui_show_screen(mode_step(current_screen, +1));
}

void ui_mode_prev(void) {
    if (current_screen == SCREEN_SPLASH) ui_show_screen(saved_mode);
    else                                 ui_show_screen(mode_step(current_screen, -1));
}

void ui_show_screen(screen_t screen) {
    if (screen >= SCREEN_COUNT) return;
    splash_hide();
    current_screen = screen;

    if (screen == SCREEN_SPLASH) splash_show();
    else                         mode_store(screen);

    const bool splash = (screen == SCREEN_SPLASH);
    if (logo_img) {
        if (splash) lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
    }
    if (lbl_title) {
        if (splash) lv_obj_add_flag(lbl_title, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_clear_flag(lbl_title, LV_OBJ_FLAG_HIDDEN);
    }

    refresh_view(true);
    apply_battery_visibility();
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    (void)name; (void)mac;
    bool was_connected = s_ble_connected;
    s_ble_connected = (state == BLE_STATE_CONNECTED);

    if (s_ble_connected && !was_connected) connected_at_ms = lv_tick_get();
    refresh_view(false);
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
