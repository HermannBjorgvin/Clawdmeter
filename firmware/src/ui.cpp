#include "ui.h"
#include "splash.h"
#include <lvgl.h>
#include "logo.h"
#include "icons.h"
#include "codex_icon.h"
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
    int16_t usage_icon_size;
    int16_t usage_metric_y;
    int16_t usage_bar_y;
    int16_t usage_reset_y;
    const lv_font_t* usage_title_font;
    const lv_font_t* usage_name_font;
    const lv_font_t* usage_pct_font;
    const lv_font_t* usage_label_font;
    const lv_font_t* usage_reset_font;

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
        L.usage_icon_size = 36;
        L.usage_metric_y = 48;
        L.usage_bar_y = 104;
        L.usage_reset_y = 124;
        L.usage_title_font = &font_tiempos_56;
        L.usage_name_font = &font_styrene_24;
        L.usage_pct_font = &font_styrene_48;
        L.usage_label_font = &font_styrene_16;
        L.usage_reset_font = &font_styrene_14;
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
        L.usage_icon_size = 30;
        L.usage_metric_y = 42;
        L.usage_bar_y = 88;
        L.usage_reset_y = 105;
        L.usage_title_font = &font_tiempos_34;
        L.usage_name_font = &font_styrene_20;
        L.usage_pct_font = &font_styrene_28;
        L.usage_label_font = &font_styrene_14;
        L.usage_reset_font = &font_styrene_12;
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
static lv_obj_t* usage_dual_container;
static lv_obj_t* usage_claude_container;
static lv_obj_t* usage_codex_container;
static lv_obj_t* lbl_anim;

struct ProviderUsageWidgets {
    lv_obj_t* panel;
    lv_obj_t* mark;
    lv_obj_t* name;
    lv_obj_t* status;
    lv_obj_t* session_label;
    lv_obj_t* session_pct;
    lv_obj_t* session_bar;
    lv_obj_t* session_reset;
    lv_obj_t* weekly_label;
    lv_obj_t* weekly_pct;
    lv_obj_t* weekly_bar;
    lv_obj_t* weekly_reset;
};
static ProviderUsageWidgets dual_widgets[USAGE_PROVIDER_COUNT];
static ProviderUsageWidgets single_widgets[USAGE_PROVIDER_COUNT];

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
static lv_image_dsc_t codex_icon_dsc;
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

static void format_reset_short(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "--");
    } else if (mins < 60) {
        snprintf(buf, len, "%dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "%dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "%dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

static bool is_usage_screen(screen_t screen) {
    return screen == SCREEN_USAGE ||
           screen == SCREEN_USAGE_CLAUDE ||
           screen == SCREEN_USAGE_CODEX;
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

static void init_battery_icons(void) {
    init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_data);
    init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_W, ICON_BATTERY_LOW_H, icon_battery_low_data);
    init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_W, ICON_BATTERY_MEDIUM_H, icon_battery_medium_data);
    init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_W, ICON_BATTERY_FULL_H, icon_battery_full_data);
    init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_W, ICON_BATTERY_CHARGING_H, icon_battery_charging_data);
}

// ======== Usage Screen ========

static void style_metric_label(lv_obj_t* lbl, const char* text) {
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, L.usage_label_font, 0);
    lv_obj_set_style_text_color(lbl, COL_DIM, 0);
}

static void make_claude_mark(lv_obj_t* panel, ProviderUsageWidgets* widgets) {
    lv_obj_t* mark = lv_obj_create(panel);
    lv_obj_set_size(mark, L.usage_icon_size, L.usage_icon_size);
    lv_obj_set_pos(mark, 0, 0);
    lv_obj_set_style_bg_opa(mark, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mark, 0, 0);
    lv_obj_set_style_pad_all(mark, 0, 0);
    lv_obj_clear_flag(mark, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* img = lv_image_create(mark);
    lv_image_set_src(img, &logo_dsc);
    lv_image_set_scale(img, (uint32_t)L.usage_icon_size * 256 / LOGO_WIDTH);
    lv_obj_center(img);
    widgets->mark = mark;
}

static void make_codex_mark(lv_obj_t* panel, ProviderUsageWidgets* widgets) {
    lv_obj_t* mark = lv_obj_create(panel);
    lv_obj_set_size(mark, L.usage_icon_size, L.usage_icon_size);
    lv_obj_set_pos(mark, 0, 0);
    lv_obj_set_style_bg_opa(mark, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mark, 0, 0);
    lv_obj_set_style_pad_all(mark, 0, 0);
    lv_obj_clear_flag(mark, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* img = lv_image_create(mark);
    lv_image_set_src(img, &codex_icon_dsc);
    lv_obj_center(img);
    widgets->mark = mark;
}

static void make_provider_usage_panel(lv_obj_t* parent, ProviderUsageWidgets* widgets,
                                      UsageProvider provider, int y, int h,
                                      const char* name, bool spacious) {
    widgets->panel = make_panel(parent, L.margin, y, L.content_w, h);
    const int inner_w = L.content_w - 32;

    if (provider == USAGE_PROVIDER_CODEX) make_codex_mark(widgets->panel, widgets);
    else                                  make_claude_mark(widgets->panel, widgets);

    widgets->name = lv_label_create(widgets->panel);
    lv_label_set_text(widgets->name, name);
    lv_obj_set_style_text_font(widgets->name, L.usage_name_font, 0);
    lv_obj_set_style_text_color(widgets->name, COL_TEXT, 0);
    lv_obj_set_pos(widgets->name, L.usage_icon_size + 10, 3);

    widgets->status = lv_label_create(widgets->panel);
    lv_label_set_text(widgets->status, "waiting");
    lv_obj_set_style_text_font(widgets->status, L.usage_reset_font, 0);
    lv_obj_set_style_text_color(widgets->status, COL_DIM, 0);
    lv_obj_align(widgets->status, LV_ALIGN_TOP_RIGHT, 0, 8);

    const int gap = 14;
    const int col_w = (inner_w - gap) / 2;
    const int col2 = col_w + gap;
    const int metric_y = spacious ? 82 : L.usage_metric_y;
    const int pct_y = metric_y + (spacious ? 22 : 14);
    const int bar_y = spacious ? h - 72 : L.usage_bar_y;
    const int reset_y = spacious ? h - 50 : L.usage_reset_y;

    widgets->session_label = lv_label_create(widgets->panel);
    style_metric_label(widgets->session_label, "5h");
    lv_obj_set_pos(widgets->session_label, 0, metric_y);

    widgets->weekly_label = lv_label_create(widgets->panel);
    style_metric_label(widgets->weekly_label, "Week");
    lv_obj_set_pos(widgets->weekly_label, col2, metric_y);

    widgets->session_pct = lv_label_create(widgets->panel);
    lv_label_set_text(widgets->session_pct, "--%");
    lv_obj_set_style_text_font(widgets->session_pct, L.usage_pct_font, 0);
    lv_obj_set_style_text_color(widgets->session_pct, COL_TEXT, 0);
    lv_obj_set_pos(widgets->session_pct, 0, pct_y);

    widgets->weekly_pct = lv_label_create(widgets->panel);
    lv_label_set_text(widgets->weekly_pct, "--%");
    lv_obj_set_style_text_font(widgets->weekly_pct, L.usage_pct_font, 0);
    lv_obj_set_style_text_color(widgets->weekly_pct, COL_TEXT, 0);
    lv_obj_set_pos(widgets->weekly_pct, col2, pct_y);

    widgets->session_bar = make_bar(widgets->panel, 0, bar_y, col_w, 12);
    widgets->weekly_bar = make_bar(widgets->panel, col2, bar_y, col_w, 12);

    widgets->session_reset = lv_label_create(widgets->panel);
    lv_label_set_text(widgets->session_reset, "--");
    lv_obj_set_style_text_font(widgets->session_reset, L.usage_reset_font, 0);
    lv_obj_set_style_text_color(widgets->session_reset, COL_DIM, 0);
    lv_obj_set_pos(widgets->session_reset, 0, reset_y);

    widgets->weekly_reset = lv_label_create(widgets->panel);
    lv_label_set_text(widgets->weekly_reset, "--");
    lv_obj_set_style_text_font(widgets->weekly_reset, L.usage_reset_font, 0);
    lv_obj_set_style_text_color(widgets->weekly_reset, COL_DIM, 0);
    lv_obj_set_pos(widgets->weekly_reset, col2, reset_y);
}

static lv_obj_t* make_usage_root(lv_obj_t* scr, const char* title) {
    lv_obj_t* root = lv_obj_create(scr);
    lv_obj_set_size(root, L.scr_w, L.scr_h);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, global_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* lbl_title = lv_label_create(root);
    lv_label_set_text(lbl_title, title);
    lv_obj_set_style_text_font(lbl_title, L.usage_title_font, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, L.title_y);
    return root;
}

static void init_usage_screen(lv_obj_t* scr) {
    usage_dual_container = make_usage_root(scr, "Usage");
    make_provider_usage_panel(usage_dual_container, &dual_widgets[USAGE_PROVIDER_CLAUDE],
                              USAGE_PROVIDER_CLAUDE, L.content_y,
                              L.usage_panel_h, "Claude", false);
    make_provider_usage_panel(usage_dual_container, &dual_widgets[USAGE_PROVIDER_CODEX],
                              USAGE_PROVIDER_CODEX,
                              L.content_y + L.usage_panel_h + L.usage_panel_gap,
                              L.usage_panel_h, "Codex", false);

    lbl_anim = lv_label_create(usage_dual_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, &font_mono_32, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -15);

    const int single_y = L.content_y + (L.scr_h >= 460 ? 20 : 12);
    const int single_h = L.scr_h - single_y - (L.scr_h >= 460 ? 86 : 78);

    usage_claude_container = make_usage_root(scr, "Claude");
    make_provider_usage_panel(usage_claude_container, &single_widgets[USAGE_PROVIDER_CLAUDE],
                              USAGE_PROVIDER_CLAUDE, single_y, single_h,
                              "Claude", true);
    lv_obj_add_flag(usage_claude_container, LV_OBJ_FLAG_HIDDEN);

    usage_codex_container = make_usage_root(scr, "Codex");
    make_provider_usage_panel(usage_codex_container, &single_widgets[USAGE_PROVIDER_CODEX],
                              USAGE_PROVIDER_CODEX, single_y, single_h,
                              "Codex", true);
    lv_obj_add_flag(usage_codex_container, LV_OBJ_FLAG_HIDDEN);
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

    lv_obj_t* lbl_ble_title = lv_label_create(ble_container);
    lv_label_set_text(lbl_ble_title, "Bluetooth");
    lv_obj_set_style_text_font(lbl_ble_title, L.bt_title_font, 0);
    lv_obj_set_style_text_color(lbl_ble_title, COL_TEXT, 0);
    lv_obj_align(lbl_ble_title, LV_ALIGN_TOP_MID, 16, L.title_y);

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

// ======== Public API ========

void ui_init(void) {
    compute_layout(board_caps());

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    init_icon_dsc_rgb565a8(&logo_dsc, LOGO_WIDTH, LOGO_HEIGHT, logo_data);
    init_icon_dsc_rgb565a8(&codex_icon_dsc, ICON_CODEX_W, ICON_CODEX_H, icon_codex_data);
    init_battery_icons();

    init_usage_screen(scr);
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

void ui_update(const UsageData* data) {
    if (!data->valid) return;

    for (int i = 0; i < USAGE_PROVIDER_COUNT; i++) {
        const ProviderUsageData* usage = &data->providers[i];
        ProviderUsageWidgets* targets[2] = {
            &dual_widgets[i],
            &single_widgets[i],
        };

        for (int j = 0; j < 2; j++) {
            ProviderUsageWidgets* widgets = targets[j];
            if (!widgets->panel) continue;
            if (!usage->valid) {
                lv_label_set_text(widgets->status, "waiting");
                lv_obj_set_style_text_color(widgets->status, COL_DIM, 0);
                lv_label_set_text(widgets->session_pct, "--%");
                lv_label_set_text(widgets->weekly_pct, "--%");
                lv_label_set_text(widgets->session_reset, "--");
                lv_label_set_text(widgets->weekly_reset, "--");
                lv_bar_set_value(widgets->session_bar, 0, LV_ANIM_ON);
                lv_bar_set_value(widgets->weekly_bar, 0, LV_ANIM_ON);
                lv_obj_set_style_bg_color(widgets->session_bar, COL_DIM, LV_PART_INDICATOR);
                lv_obj_set_style_bg_color(widgets->weekly_bar, COL_DIM, LV_PART_INDICATOR);
                continue;
            }

            int s_pct = (int)(usage->session_pct + 0.5f);
            int w_pct = (int)(usage->weekly_pct + 0.5f);
            lv_label_set_text_fmt(widgets->session_pct, "%d%%", s_pct);
            lv_label_set_text_fmt(widgets->weekly_pct, "%d%%", w_pct);
            lv_bar_set_value(widgets->session_bar, s_pct, LV_ANIM_ON);
            lv_bar_set_value(widgets->weekly_bar, w_pct, LV_ANIM_ON);
            lv_obj_set_style_bg_color(widgets->session_bar, pct_color(usage->session_pct),
                                      LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(widgets->weekly_bar, pct_color(usage->weekly_pct),
                                      LV_PART_INDICATOR);

            char buf[24];
            format_reset_short(usage->session_reset_mins, buf, sizeof(buf));
            lv_label_set_text(widgets->session_reset, buf);
            format_reset_short(usage->weekly_reset_mins, buf, sizeof(buf));
            lv_label_set_text(widgets->weekly_reset, buf);

            lv_label_set_text(widgets->status, usage->ok ? usage->status : "error");
            lv_obj_set_style_text_color(widgets->status, usage->ok ? COL_GREEN : COL_RED, 0);
        }
    }
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
    lv_obj_add_flag(usage_dual_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usage_claude_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usage_codex_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ble_container, LV_OBJ_FLAG_HIDDEN);
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:       splash_show(); break;
    case SCREEN_USAGE:        lv_obj_clear_flag(usage_dual_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_USAGE_CLAUDE: lv_obj_clear_flag(usage_claude_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_USAGE_CODEX:  lv_obj_clear_flag(usage_codex_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_BLUETOOTH:    lv_obj_clear_flag(ble_container, LV_OBJ_FLAG_HIDDEN); break;
    default: break;
    }

    if (logo_img) {
        if (screen == SCREEN_SPLASH || is_usage_screen(screen)) {
            lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
    apply_battery_visibility();
}

void ui_cycle_screen(void) {
    screen_t next;
    switch (current_screen) {
    case SCREEN_USAGE:        next = SCREEN_USAGE_CLAUDE; break;
    case SCREEN_USAGE_CLAUDE: next = SCREEN_USAGE_CODEX;  break;
    case SCREEN_USAGE_CODEX:  next = SCREEN_USAGE;        break;
    default:                  next = SCREEN_USAGE;        break;
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
