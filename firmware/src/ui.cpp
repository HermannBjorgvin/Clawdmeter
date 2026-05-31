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
    int16_t usage_bar_h;
    int16_t panel_hpad;       // make_panel pad_left == pad_right
    int16_t panel_vpad;       // make_panel pad_top  == pad_bottom
    const lv_font_t* usage_title_font;
    const lv_font_t* usage_pct_font;
    const lv_font_t* usage_pill_font;
    const lv_font_t* usage_reset_font;
    const lv_font_t* usage_anim_font;

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

    if (c.height >= 460) {
        // Large layout — tuned for 480x480 (AMOLED-2.16).
        L.margin = 20;
        L.title_y = 30;
        L.content_y = 100;
        L.usage_panel_h = 150;
        L.usage_panel_gap = 16;
        L.usage_bar_y = 56;
        L.usage_reset_y = 94;
        L.usage_bar_h = 24;
        L.panel_hpad = 16;
        L.panel_vpad = 12;
        L.usage_title_font = &font_tiempos_56;
        L.usage_pct_font   = &font_styrene_48;
        L.usage_pill_font  = &font_styrene_28;
        L.usage_reset_font = &font_styrene_28;
        L.usage_anim_font  = &font_mono_32;
        L.bt_info_panel_h = 160;
        L.bt_reset_zone_h = 110;
        L.bt_title_font    = &font_tiempos_56;
        L.bt_status_font   = &font_styrene_48;
        L.bt_device_font   = &font_styrene_28;
        L.bt_credit_1_font = &font_styrene_24;
        L.bt_credit_2_font = &font_styrene_20;
    } else if (c.height >= 250) {
        // Compact layout — tuned for 368x448 (AMOLED-1.8).
        L.margin = 20;
        L.title_y = 30;
        L.content_y = 85;
        L.usage_panel_h = 130;
        L.usage_panel_gap = 12;
        L.usage_bar_y = 48;
        L.usage_reset_y = 78;
        L.usage_bar_h = 24;
        L.panel_hpad = 16;
        L.panel_vpad = 12;
        L.usage_title_font = &font_tiempos_56;
        L.usage_pct_font   = &font_styrene_48;
        L.usage_pill_font  = &font_styrene_28;
        L.usage_reset_font = &font_styrene_28;
        L.usage_anim_font  = &font_mono_32;
        L.bt_info_panel_h = 140;
        L.bt_reset_zone_h = 90;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_28;
        L.bt_device_font   = &font_styrene_20;
        L.bt_credit_1_font = &font_styrene_16;
        L.bt_credit_2_font = &font_styrene_14;
    } else {
        // Tiny layout — tuned for 200x200 e-paper (Waveshare 1.54 V2).
        // Keeps every element from the AMOLED layout (logo, battery,
        // both usage panels, rotating animation message) but with
        // shrunk fonts, scaled icons, and tighter spacing so the whole
        // thing fits on a 200px-tall panel. ui_init applies
        // lv_image_set_scale to the logo (~37 %) and battery (~50 %).
        //
        // Vertical budget (200 px total):
        //   y=0..30   top row: logo (30 px), title centred, battery (24 px)
        //   y=34..98  panel 1 (Current) - 64 px
        //   y=102..166 panel 2 (Weekly) - 64 px
        //   y=170..200 footer: rotating animation message
        L.margin = 6;
        // title_y=4 lines up the visible top of the "Usage" /
        // "Bluetooth" header glyphs with the visible-content top of
        // the Claude logo and battery icons (both 0-based on this
        // tier but have a few px of transparent padding at the top of
        // their source bitmaps before the first painted pixel). The
        // styrene_20 line-box at y=4 spans y=4..24 — visible glyphs
        // start around y=6-7, matching where the icons' visible ink
        // begins.
        L.title_y = 4;
        L.content_y = 34;
        L.usage_panel_h = 64;
        L.usage_panel_gap = 4;
        // Within each 64 px panel (1 px pad top+bottom, 62 px usable):
        //   child y=0..28  pct (styrene_28, line_height 28)
        //   child y=32..42 bar (10 px, 4 px gap above)
        //   child y=46..62 reset (styrene_16, line_height 16, 4 px gap)
        L.usage_bar_y = 32;
        L.usage_reset_y = 46;
        L.usage_bar_h = 10;
        L.panel_hpad = 6;
        L.panel_vpad = 1;
        // Match the Bluetooth screen's title size (L.bt_title_font =
        // styrene_20) so the two screens read at the same visual weight.
        L.usage_title_font = &font_styrene_20;
        L.usage_pct_font   = &font_styrene_28;
        // styrene_16 (was 14) so the "Current"/"Weekly" pill reads
        // slightly bigger and balances better against the styrene_28
        // percentage next to it. styrene_16 is the next available
        // size between 14 and 20.
        L.usage_pill_font  = &font_styrene_16;
        L.usage_reset_font = &font_styrene_16;
        // font_mono_18 (DejaVuSansMono) was generated with the U+27xx
        // spinner glyphs and U+2026 ellipsis included; the proportional
        // Styrene fonts are ASCII-only. Using mono here on the tiny
        // tier brings back the original Unicode spinner aesthetic at
        // the cost of a slightly different (monospaced) typeface for
        // the footer line — accepted trade-off documented in the
        // commit message.
        L.usage_anim_font  = &font_mono_18;
        L.bt_info_panel_h = 100;
        L.bt_reset_zone_h = 60;
        L.bt_title_font    = &font_styrene_20;
        L.bt_status_font   = &font_styrene_14;
        // styrene_12 (not 14) so "Address: 70:04:1D:DB:CC:89" — 25 chars
        // averaging ~8 px each at styrene_14 — fits on a single line in
        // the 188 px content area. Also used for the "Device:" line and
        // the "Reset Bluetooth" label so they share visual weight.
        L.bt_device_font   = &font_styrene_12;
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

// Decorative spinner glyphs: U+00B7 middle dot + U+2722/2733/2736/273B/
// 273D Dingbats stars. Both anim fonts (font_mono_32 on AMOLED,
// font_mono_18 on the tiny tier — DejaVuSansMono in both cases) were
// generated with these codepoints in their glyph range, so the spinner
// renders properly on every board.
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
    // On the tiny e-paper tier the display HAL inverts pixel luminance,
    // and only COL_AMBER lands cleanly on the panel-black side of the
    // threshold (COL_RED inverts to invisible-white; COL_GREEN is on
    // the edge). Forcing the indicator to a high-luminance text colour
    // makes the filled portion render as a solid panel-black bar
    // regardless of rate-group, paired with the bar's high-luminance
    // border for a clean black-outline + black-fill paper-style bar.
    if (L.scr_h < 250) return COL_TEXT;
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

static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    // Panel padding comes from the active tier (tiny tier uses tighter
    // values so the pct + bar + reset fit inside a 64 px panel).
    lv_obj_set_style_pad_left(panel, L.panel_hpad, 0);
    lv_obj_set_style_pad_right(panel, L.panel_hpad, 0);
    lv_obj_set_style_pad_top(panel, L.panel_vpad, 0);
    lv_obj_set_style_pad_bottom(panel, L.panel_vpad, 0);
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
    // Tiny tier (e-paper): give the bar a high-luminance border so the
    // display-HAL inversion renders a clean BLACK outline of the full
    // bar extent on the white panel. Without this the unfilled portion
    // of the bar (COL_BAR_BG, very dark, inverts to panel-white) is
    // invisible and a low-percentage filled bar looks like a tiny smear
    // at the left edge.
    if (L.scr_h < 250) {
        lv_obj_set_style_border_color(bar, COL_TEXT, LV_PART_MAIN);
        lv_obj_set_style_border_width(bar, 2, LV_PART_MAIN);
        lv_obj_set_style_border_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    }
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
    lv_obj_set_style_text_font(lbl, L.usage_pill_font, 0);
    // AMOLED: a dim COL_BAR_BG chip with light COL_TEXT on top. On the
    // tiny e-paper tier the 1bpp HAL inverts luminance, so a dark
    // COL_BAR_BG fill would vanish into the white paper — invert the pill
    // (light fill, dark text in LVGL) so it renders as a solid BLACK pill
    // with WHITE "Current"/"Weekly" text on the panel.
    const bool pill_invert = (L.scr_h < 250);
    lv_obj_set_style_bg_color(lbl, pill_invert ? COL_TEXT : COL_BAR_BG, 0);
    lv_obj_set_style_text_color(lbl, pill_invert ? COL_BG : COL_TEXT, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl, LV_RADIUS_CIRCLE, 0);
    // Pill padding scales with the font: keep tight on the tiny tier so
    // "Current" / "Weekly" fits next to the percentage without clipping.
    const int hpad = (L.scr_h < 250) ? 8 : 18;
    const int vpad = (L.scr_h < 250) ? 2 : 6;
    lv_obj_set_style_pad_left(lbl, hpad, 0);
    lv_obj_set_style_pad_right(lbl, hpad, 0);
    lv_obj_set_style_pad_top(lbl, vpad, 0);
    lv_obj_set_style_pad_bottom(lbl, vpad, 0);
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

static void make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                             lv_obj_t** out_pct, lv_obj_t** out_pill,
                             lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, L.margin, y, L.content_w, L.usage_panel_h);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, L.usage_pct_font, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text);
    // AMOLED tiers keep the original +1 nudge (unchanged from before the
    // e-paper port). The tiny tier centres the pill's line-box against the
    // much taller pct glyph so the two read on one baseline:
    //   tiny : pct=28, pill=16+4 pad → offset (28-20)/2 = 4
    const bool pill_tiny = (L.scr_h < 250);
    const int  pill_vpad = pill_tiny ? 2 : 6;
    const int  pct_h     = lv_font_get_line_height(L.usage_pct_font);
    const int  pill_h    = lv_font_get_line_height(L.usage_pill_font)
                         + 2 * pill_vpad;
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0,
                 pill_tiny ? (pct_h - pill_h) / 2 : 1);

    // Bar fills the panel's full content width (panel total minus both
    // sides' padding). The previous "- 32" was hardcoded for AMOLED
    // padding (16+16) and left ~20 px of dead space on the right of
    // the tiny tier (6+6 padding).
    *out_bar = make_bar(panel, 0, L.usage_bar_y,
                       L.content_w - 2 * L.panel_hpad, L.usage_bar_h);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, L.usage_reset_font, 0);
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
    lv_obj_set_style_text_font(lbl_title, L.usage_title_font, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    // On AMOLED the title is shifted +16 to clear the 80×80 top-left
    // logo overlay. On the tiny tier the logo is scaled to ~30 px and
    // doesn't reach the title, so center cleanly.
    const bool tiny_title = (L.scr_h < 250);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID,
                 tiny_title ? 0 : 16, L.title_y);

    make_usage_panel(usage_container, L.content_y, "Current",
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset);
    make_usage_panel(usage_container,
                     L.content_y + L.usage_panel_h + L.usage_panel_gap, "Weekly",
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset);

    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, L.usage_anim_font, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    if (L.scr_h < 250) {
        // Tiny tier uses the proportional 18-px DejaVuSansMono for the
        // anim label (it's the only available font that carries the
        // U+27xx spinner glyphs). The longest messages — e.g.
        // "Flibbertigibbeting" — overflow the panel width at that
        // size, so give the label a fixed width and let LVGL truncate
        // gracefully with its own "..." marker instead of running off
        // the right edge. Width = L.content_w with LV_ALIGN_BOTTOM_MID
        // puts the widget at x=L.margin..(scr_w-L.margin), text
        // centered inside — visually flanked by equal margins on each
        // side.
        lv_obj_set_width(lbl_anim, L.content_w);
        lv_obj_set_style_text_align(lbl_anim, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(lbl_anim, LV_LABEL_LONG_MODE_DOTS);

        // Vertically centre the label in the free space between the
        // bottom of the second usage panel and the bottom of the
        // screen. font_mono_18 reports a line height of ~18 px, so
        // half-height ≈ 9.
        const int weekly_bottom = L.content_y + 2 * L.usage_panel_h
                                + L.usage_panel_gap;
        const int free_center   = (weekly_bottom + L.scr_h) / 2;
        const int anim_half_h   = 9;
        const int from_bottom   = L.scr_h - free_center - anim_half_h;
        lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -from_bottom);
    } else {
        lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -15);
    }
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
    lv_obj_add_event_cb(ble_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    const bool tiny = (L.scr_h < 250);
    const lv_color_t dim_text = COL_DIM;

    static lv_image_dsc_t icon_bt_dsc;
    init_icon_dsc(&icon_bt_dsc, ICON_BLUETOOTH_W, ICON_BLUETOOTH_H, icon_bluetooth_data);
    static lv_image_dsc_t icon_trash_dsc;
    init_icon_dsc(&icon_trash_dsc, ICON_TRASH2_W, ICON_TRASH2_H, icon_trash2_data);

    lv_obj_t* lbl_ble_title = lv_label_create(ble_container);
    lv_label_set_text(lbl_ble_title, "Bluetooth");
    lv_obj_set_style_text_font(lbl_ble_title, L.bt_title_font, 0);
    lv_obj_set_style_text_color(lbl_ble_title, COL_TEXT, 0);
    lv_obj_align(lbl_ble_title, LV_ALIGN_TOP_MID,
                 tiny ? 0 : 16, L.title_y);

    if (tiny) {
        // -------- 200×200 BT layout --------
        // The shared make_panel() background (COL_PANEL=0x1f1f1e) inverts
        // to invisible-white on the e-paper, so the "card" framing the
        // AMOLED layout uses adds nothing here. Place items directly on
        // the container with explicit positions.
        //
        // Vertical budget (200 px total, top icon row occupies y=0..30):
        //   y=34..66   BT icon (28×28 scaled from 48×48) + status text
        //   y=72..86   Device: <name>
        //   y=90..104  Address: <mac>
        //   y=112..136 Reset row (trash 22×22 + "Reset Bluetooth")
        //   y=156..168 Credit line 1 (styrene_12)
        //   y=170..182 Credit line 2 (styrene_12)
        const int bt_icon_size  = 28;
        const int bt_icon_y     = L.content_y;

        // BT icon + status share a centred flex row instead of being
        // positioned by hand, so the pair sits visually under the
        // centred "Bluetooth" title rather than left-jutting from the
        // margin.
        lv_obj_t* bt_row = lv_obj_create(ble_container);
        lv_obj_set_pos(bt_row, 0, bt_icon_y);
        lv_obj_set_size(bt_row, L.scr_w, bt_icon_size);
        lv_obj_set_style_bg_opa(bt_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(bt_row, 0, 0);
        lv_obj_set_style_pad_all(bt_row, 0, 0);
        lv_obj_set_style_pad_column(bt_row, 6, 0);
        lv_obj_set_flex_flow(bt_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(bt_row, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(bt_row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(bt_row, LV_OBJ_FLAG_EVENT_BUBBLE);

        lv_obj_t* bt_img = lv_image_create(bt_row);
        lv_image_set_src(bt_img, &icon_bt_dsc);
        // LV_IMAGE_ALIGN_CONTAIN + an explicit bbox size lets LVGL
        // auto-scale the 48 × 48 source to fit the 28 × 28 widget
        // while preserving aspect — cleaner than combining
        // lv_image_set_scale with lv_obj_set_size (which double-scales
        // because the size constraint AND the scale value both
        // shrink the image, and the prior 28×28 bbox+0.58× scale
        // collapsed the BT icon to ~16 px).
        lv_obj_set_size(bt_img, bt_icon_size, bt_icon_size);
        lv_image_set_inner_align(bt_img, LV_IMAGE_ALIGN_CONTAIN);

        lbl_ble_status = lv_label_create(bt_row);
        lv_label_set_text(lbl_ble_status, "Initializing...");
        lv_obj_set_style_text_font(lbl_ble_status, L.bt_status_font, 0);
        lv_obj_set_style_text_color(lbl_ble_status, dim_text, 0);

        // LVGL's LV_LABEL_LONG_MODE_DOTS only adds "..." when the text
        // exceeds the label's *bounding box* (both width AND height); if
        // height is auto, the label grows vertically and the text wraps
        // instead. Lock height to one line height so any future-long
        // device/MAC/credit text truncates cleanly rather than wrapping
        // off-screen.
        const int device_line_h = 14;  // styrene_12 line height + 2 px
        const int credit_line_h = 14;  // styrene_12 line height + 2 px

        const int device_y = bt_icon_y + bt_icon_size + 8;
        lbl_ble_device = lv_label_create(ble_container);
        lv_label_set_text(lbl_ble_device, "Device: ---");
        lv_obj_set_style_text_font(lbl_ble_device, L.bt_device_font, 0);
        lv_obj_set_style_text_color(lbl_ble_device, dim_text, 0);
        lv_obj_set_size(lbl_ble_device, L.content_w, device_line_h);
        // Centre the device + MAC text so they line up under the
        // centred "Bluetooth" title instead of jutting out left.
        lv_obj_set_style_text_align(lbl_ble_device, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(lbl_ble_device, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_pos(lbl_ble_device, L.margin, device_y);

        const int mac_y = device_y + device_line_h + 2;
        lbl_ble_mac = lv_label_create(ble_container);
        lv_label_set_text(lbl_ble_mac, "Address: ---");
        lv_obj_set_style_text_font(lbl_ble_mac, L.bt_device_font, 0);
        lv_obj_set_style_text_color(lbl_ble_mac, dim_text, 0);
        lv_obj_set_size(lbl_ble_mac, L.content_w, device_line_h);
        lv_obj_set_style_text_align(lbl_ble_mac, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(lbl_ble_mac, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_pos(lbl_ble_mac, L.margin, mac_y);

        // Reset row — trash icon at the same 28×28 size as the BT icon
        // up top so the two icons feel like a matched pair. "Reset
        // Bluetooth" label next to it, both inside a transparent flex
        // row that's the click target for ble_reset_click_cb.
        //
        // Extra vertical padding on the row (+8 instead of +4) and a
        // larger gap above it (+18 instead of +8) so the trash icon's
        // top strokes aren't clipped by the row's edges. Row height
        // works out to 36 px, leaving 4 px of clearance above the
        // first credit line at y=158.
        const int reset_y    = mac_y + device_line_h + 18;
        const int trash_size = bt_icon_size;   // match the BT icon size
        lv_obj_t* reset_zone = lv_obj_create(ble_container);
        lv_obj_set_pos(reset_zone, L.margin, reset_y);
        lv_obj_set_size(reset_zone, L.content_w, trash_size + 8);
        lv_obj_set_style_bg_opa(reset_zone, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(reset_zone, 0, 0);
        lv_obj_set_style_pad_all(reset_zone, 0, 0);
        lv_obj_set_style_pad_column(reset_zone, 6, 0);
        lv_obj_set_flex_flow(reset_zone, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(reset_zone, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(reset_zone, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(reset_zone, ble_reset_click_cb,
                            LV_EVENT_CLICKED, NULL);

        lv_obj_t* trash_img = lv_image_create(reset_zone);
        lv_image_set_src(trash_img, &icon_trash_dsc);
        // Same CONTAIN-based scaling as the BT icon above — LVGL
        // resizes the 48 × 48 source to fit the 28 × 28 widget while
        // preserving aspect ratio, no manual scale math needed.
        lv_obj_set_size(trash_img, trash_size, trash_size);
        lv_image_set_inner_align(trash_img, LV_IMAGE_ALIGN_CONTAIN);

        lv_obj_t* reset_lbl = lv_label_create(reset_zone);
        lv_label_set_text(reset_lbl, "Reset Bluetooth");
        lv_obj_set_style_text_font(reset_lbl, L.bt_device_font, 0);
        lv_obj_set_style_text_color(reset_lbl, dim_text, 0);

        // Credits at the bottom in styrene_12, both centred. Credit-1
        // ("Built by @hermannbjorgvin") fits on one line; credit-2
        // ("Clawd animation by @amaanbuilds") wraps to two lines —
        // height is set to two line heights and LV_LABEL_LONG_MODE_WRAP
        // (LVGL default) is used so the "@amaanbuilds" continuation
        // lands on its own line inside the bbox instead of being
        // truncated.
        const int credit2_h = 2 * credit_line_h;
        const int credit2_y = L.scr_h - credit2_h;
        const int credit1_y = credit2_y - credit_line_h;

        lv_obj_t* lbl_credit = lv_label_create(ble_container);
        lv_label_set_text(lbl_credit, "Built by @hermannbjorgvin");
        lv_obj_set_style_text_font(lbl_credit, L.bt_credit_1_font, 0);
        lv_obj_set_style_text_color(lbl_credit, dim_text, 0);
        lv_obj_set_size(lbl_credit, L.content_w, credit_line_h);
        lv_obj_set_style_text_align(lbl_credit, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(lbl_credit, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_pos(lbl_credit, L.margin, credit1_y);

        lv_obj_t* lbl_credit2 = lv_label_create(ble_container);
        lv_label_set_text(lbl_credit2, "Clawd animation by @amaanbuilds");
        lv_obj_set_style_text_font(lbl_credit2, L.bt_credit_2_font, 0);
        lv_obj_set_style_text_color(lbl_credit2, dim_text, 0);
        lv_obj_set_size(lbl_credit2, L.content_w, credit2_h);
        lv_obj_set_style_text_align(lbl_credit2, LV_TEXT_ALIGN_CENTER, 0);
        // Default long mode (WRAP) so "@amaanbuilds" flows to line two
        // when "Clawd animation by " fills line one.
        lv_obj_set_pos(lbl_credit2, L.margin, credit2_y);
    } else {
        // -------- AMOLED BT layout (unchanged) --------
        lv_obj_t* p_info = make_panel(ble_container, L.margin, L.content_y,
                                      L.content_w, L.bt_info_panel_h);

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
    }

    lv_obj_add_flag(ble_container, LV_OBJ_FLAG_HIDDEN);
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
    init_bluetooth_screen(scr);
    splash_init(scr);

    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    // Logo + battery icons. On the tiny tier the source images (80x80
    // logo, 48x48 battery) overwhelm a 200 px panel, so apply LVGL's
    // built-in scaling (256 = 1.0x) to shrink them to ~30 / ~24 px and
    // tuck the logo and battery into the top corners flanking the
    // "Usage" title. Native size on AMOLED tiers.
    //
    // lv_image's scale operates around the image pivot (default
    // center); without overriding the pivot, a scaled 80x80 image
    // renders centered inside its 80x80 bbox with empty padding, so
    // positioning by top-left coordinates doesn't match what's drawn.
    // Pinning pivot to (0,0) anchors the scaled image at the widget's
    // top-left so set_pos coordinates match the visible top-left
    // corner. (At 1.0x scale this is identity; safe to apply on AMOLED.)
    const bool tiny = (L.scr_h < 250);
    const uint32_t logo_scale    = tiny ? 96  : 256;   // 80 -> 30
    const uint32_t battery_scale = tiny ? 128 : 256;   // 48 -> 24
    const int battery_w = (ICON_BATTERY_W * (int)battery_scale) / 256;

    logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, &logo_dsc);
    lv_image_set_pivot(logo_img, 0, 0);
    lv_image_set_scale(logo_img, logo_scale);
    lv_obj_set_pos(logo_img, L.margin, tiny ? 0 : (L.title_y - 10));

    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    lv_image_set_pivot(battery_img, 0, 0);
    lv_image_set_scale(battery_img, battery_scale);
    lv_obj_set_pos(battery_img, L.scr_w - battery_w - L.margin,
                   tiny ? 0 : L.title_y);
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

    const uint32_t msg_interval     = ANIM_MSG_MS;
    const uint32_t spinner_interval = spinner_ms[anim_spinner_idx];

    if (now - anim_msg_start >= msg_interval) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_msg_start = now;
    }

    if (now - anim_last_ms >= spinner_interval) {
        anim_last_ms = now;
        anim_phase = (anim_phase + 1) % SPINNER_PHASES;
        anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                        : (SPINNER_PHASES - anim_phase);

        static char buf[80];
        // The U+2026 ellipsis (\xE2\x80\xA6) lives in both anim fonts'
        // glyph range — see the spinner_frames comment above.
        snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
                 spinner_frames[anim_spinner_idx],
                 anim_messages[anim_msg_idx]);
        lv_label_set_text(lbl_anim, buf);
    }
}

static screen_t prev_non_splash_screen = SCREEN_USAGE;
static void apply_battery_visibility(void) {
    if (!battery_img) return;
    // Hide on the splash screen (it's full-bleed pixel art).
    if (current_screen == SCREEN_SPLASH)
        lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
}

static void global_click_cb(lv_event_t* e) {
    (void)e;
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

static void ble_reset_click_cb(lv_event_t* e) {
    (void)e;
    ble_clear_bonds();
}

void ui_show_screen(screen_t screen) {
    lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ble_container, LV_OBJ_FLAG_HIDDEN);
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:     splash_show(); break;
    case SCREEN_USAGE:      lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_BLUETOOTH:  lv_obj_clear_flag(ble_container, LV_OBJ_FLAG_HIDDEN); break;
    default: break;
    }

    if (logo_img) {
        // Hide the logo on the splash screen (full-bleed); show it elsewhere.
        if (screen == SCREEN_SPLASH)
            lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
    }

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
    apply_battery_visibility();
}

void ui_cycle_screen(void) {
    screen_t next;
    switch (current_screen) {
    case SCREEN_USAGE:     next = SCREEN_BLUETOOTH; break;
    case SCREEN_BLUETOOTH: next = SCREEN_USAGE;     break;
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
