#include "ui.h"
#include "splash.h"
#include <lvgl.h>
#include <string.h>
#include <time.h>
#include "ble.h"
#include "usage_rate.h"
#include "logo.h"
#include "icons.h"
#include "display_cfg.h"

// Custom fonts (scaled for 135x240 1.14" IPS)
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_styrene_12);
LV_FONT_DECLARE(font_mono_18);

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

// ---- Layout constants for 135x240 (ST7789 IPS, 1.14") ----
#define SCR_W         135
#define SCR_H         240
#define MARGIN        6
#define TITLE_Y       8
#define CONTENT_Y     48
#define CONTENT_W     (SCR_W - 2 * MARGIN)   // 123

// ---- Usage screen widgets ----
static lv_obj_t* usage_container;
static lv_obj_t* lbl_model;          // "Sonnet - ctx 62%" under the title
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

// ---- Copilot screen widgets ----
static lv_obj_t* copilot_container;
static lv_obj_t* lbl_copilot_accept_pct;
static lv_obj_t* bar_copilot;
static lv_obj_t* lbl_copilot_detail;
static lv_obj_t* lbl_copilot_suggest;
static lv_obj_t* lbl_copilot_status;

// ---- Sysinfo screen widgets ----
static lv_obj_t* sysinfo_container;
static lv_obj_t* lbl_cpu_pct;
static lv_obj_t* bar_cpu;
static lv_obj_t* lbl_cpu_detail;
static lv_obj_t* lbl_ram_pct;
static lv_obj_t* bar_ram;
static lv_obj_t* lbl_ram_detail;
static lv_obj_t* lbl_disk_pct;
static lv_obj_t* bar_disk;
static lv_obj_t* lbl_disk_detail;

// ---- VS Code screen widgets ----
static lv_obj_t* vscode_container;
static lv_obj_t* lbl_vscode_mem;
static lv_obj_t* bar_vscode_mem;
static lv_obj_t* lbl_vscode_mem_detail;
static lv_obj_t* lbl_vscode_ext;
static lv_obj_t* bar_vscode_ext;
static lv_obj_t* lbl_vscode_ext_detail;
static lv_obj_t* lbl_vscode_err;
static lv_obj_t* bar_vscode_err;
static lv_obj_t* lbl_vscode_err_detail;

// ---- Clock / weather screen widgets ----
static lv_obj_t* clock_container;
static lv_obj_t* lbl_clock_time;
static lv_obj_t* lbl_clock_date;
static lv_obj_t* wx_icon;            // colour-coded condition dot
static lv_obj_t* lbl_wx_temp;
static lv_obj_t* lbl_wx_deg;         // small degree ring after the outdoor temp
static lv_obj_t* lbl_wx_cond;
static lv_obj_t* lbl_wx_hilo;
static lv_obj_t* lbl_wx_loc;
static lv_obj_t* lbl_clock_intemp;    // indoor temp, same line/size as the outdoor temp
static lv_obj_t* lbl_in_deg;          // small degree ring after the indoor temp
static lv_obj_t* lbl_clock_indoor;    // env-sensor pressure readout: "1013 hPa" (+ "  45%")
static lv_obj_t* bar_strip_claude;   // bottom "overall usage" strip
static lv_obj_t* bar_strip_copilot;

// Clock state — set by ui_update_env(), advanced locally between payloads.
static long     env_epoch      = 0;   // UTC seconds at last payload
static uint32_t env_rx_millis  = 0;   // millis() at that moment
static int      env_tz_off_min = 0;
static bool     env_time_valid = false;
static int      last_shown_min = -1;  // avoid redundant label redraws

// Focus timer (Clock screen, long-press). Owns the clock labels while running.
enum tmr_state_t { TMR_OFF, TMR_FOCUS, TMR_BREAK };
static tmr_state_t g_tmr = TMR_OFF;
static uint32_t    tmr_end_ms = 0;
static int         tmr_round = 0;
#define TMR_FOCUS_MS  (25UL * 60 * 1000)
#define TMR_BREAK_MS  (5UL  * 60 * 1000)

// Cached percentages for the bottom strip (also shown on their own screens).
static int g_session_pct = -1;
static int g_premium_pct = -1;
static int g_session_reset_mins = -1;   // from the last claude payload

// ---- Claude Code activity signal ({"src":"act"}) ----
enum act_state_t { ACT_UNKNOWN, ACT_IDLE, ACT_WORKING, ACT_NEEDS_INPUT, ACT_DONE };
static act_state_t g_act = ACT_UNKNOWN;
static int         g_act_agents = 0;
static lv_obj_t*   act_dot = NULL;       // header activity glyph (shared, on scr)
static lv_obj_t*   banner = NULL;        // "Claude needs you" overlay (shared, on scr)
static lv_obj_t*   lbl_banner = NULL;

// ---- Battery indicator (shared, on top) ----
static lv_obj_t* battery_img;
static lv_obj_t* logo_img;
static lv_image_dsc_t battery_dscs[5];  // empty, low, medium, full, charging

// ---- Data-freshness indicator (shared, top-right) ----
static lv_obj_t* lbl_status_corner = NULL;
static uint32_t  last_data_ms = 0;      // lv_tick when the last BLE payload landed
static bool      ever_data = false;     // true once any payload has been parsed
static char      daemon_state[16] = ""; // last {"src":"status"} state, "" = none/ok
#define STALE_MS       180000UL         // 3 min without data → caution
#define VERY_STALE_MS  600000UL         // 10 min without data → treat as offline

// ---- Button-press flash overlay (shared, topmost) ----
static lv_obj_t* flash_overlay;

// ---- Shared ----
static lv_image_dsc_t logo_dsc;
static screen_t current_screen = SCREEN_USAGE;
// True once the daemon has ever sent sysinfo/vscode data — lets
// ui_cycle_screen() skip diagnostic screens that have nothing to show.
static bool sysinfo_has_data = false;
static bool vscode_has_data = false;

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

// Per-frame hold time. Modeled on Claude Code's spinner (Cavalry triangle
// oscillator, range 0..5, period 5s) — turn-around frames (0 and 5) appear
// once per cycle, middle frames twice, so 0/5 read as held longer.
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

// Copilot-flavoured animation messages (used on SCREEN_COPILOT)
// (removed — replaced by pixel-art animation via splash_copilot_tick)

static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

static void format_reset_time(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "Resets %dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "Resets %dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "Resets %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

static void refresh_status_label(void);
static void refresh_clock(bool force);
static void refresh_usage_strip(void);
static void banner_set(bool show);

static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, 8, 0);
    lv_obj_set_style_pad_right(panel, 8, 0);
    lv_obj_set_style_pad_top(panel, 8, 0);
    lv_obj_set_style_pad_bottom(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    // Bubble click events up to the screen / usage_container so a tap anywhere
    // on the panel fires the global click handler.
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

// RGB565A8: planar — w*h RGB565 pixels followed by w*h alpha bytes.
// Stride is RGB565-only (w*2); LVGL infers alpha plane location from header.
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
    lv_obj_set_style_text_font(lbl, &font_styrene_12, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl, 6, 0);
    lv_obj_set_style_pad_right(lbl, 6, 0);
    lv_obj_set_style_pad_top(lbl, 3, 0);
    lv_obj_set_style_pad_bottom(lbl, 3, 0);
    return lbl;
}

// ---- Battery icon initialization ----
static void init_battery_icons(void) {
    init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_data);
    init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_W, ICON_BATTERY_LOW_H, icon_battery_low_data);
    init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_W, ICON_BATTERY_MEDIUM_H, icon_battery_medium_data);
    init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_W, ICON_BATTERY_FULL_H, icon_battery_full_data);
    init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_W, ICON_BATTERY_CHARGING_H, icon_battery_charging_data);
}

// Full-screen transparent container shared by every non-splash screen: same
// size/position/style every time, with a compact top-left title label. The
// board has no touch digitizer, so there are no tap handlers — all input is
// the single GPIO0 button (see main.cpp).
static lv_obj_t* make_screen_container(lv_obj_t* scr, const char* title) {
    lv_obj_t* c = lv_obj_create(scr);
    lv_obj_set_size(c, SCR_W, SCR_H);
    lv_obj_set_pos(c, 0, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    if (title) {
        lv_obj_t* lbl = lv_label_create(c);
        lv_label_set_text(lbl, title);
        lv_obj_set_style_text_font(lbl, &font_styrene_16, 0);
        lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
        lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, MARGIN, TITLE_Y);
    }
    return c;
}

// ======== Usage Screen (135x240) ========

#define PANEL_H     74
#define PANEL_GAP   8
// Copilot screen uses smaller 60px panels to leave room for the pixel-art animation
#define COPILOT_PANEL_H   60
#define COPILOT_PANEL_GAP  6

// One Session/Weekly panel: big % label, pill on the right, bar, reset label.
static void make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                             lv_obj_t** out_pct, lv_obj_t** out_pill,
                             lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, MARGIN, y, CONTENT_W, PANEL_H);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, &font_styrene_24, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, 28, CONTENT_W - 16, 12);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, &font_styrene_12, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, 42);
}

// Copilot panel: 60px tall (bar at y=22, detail label at y=36).
static void make_copilot_panel(lv_obj_t* parent, int y, const char* pill_text,
                               lv_obj_t** out_pct, lv_obj_t** out_pill,
                               lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, MARGIN, y, CONTENT_W, COPILOT_PANEL_H);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, &font_styrene_24, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, 22, CONTENT_W - 16, 12);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, &font_styrene_12, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, 36);
}

static void init_usage_screen(lv_obj_t* scr) {
    usage_container = make_screen_container(scr, "Claude");

    lbl_model = lv_label_create(usage_container);
    lv_label_set_text(lbl_model, "");
    lv_obj_set_style_text_font(lbl_model, &font_styrene_12, 0);
    lv_obj_set_style_text_color(lbl_model, COL_DIM, 0);
    lv_obj_align(lbl_model, LV_ALIGN_TOP_LEFT, MARGIN, 28);

    make_usage_panel(usage_container, CONTENT_Y, "Current",
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset);
    make_usage_panel(usage_container, CONTENT_Y + PANEL_H + PANEL_GAP, "Weekly",
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset);

    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, &font_mono_18, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -8);
}

// ======== Copilot Screen (135x240) ========

static void init_copilot_screen(lv_obj_t* scr) {
    copilot_container = make_screen_container(scr, "Copilot");

    // Panel 1 — premium request usage %
    {
        lv_obj_t* _pill;
        make_copilot_panel(copilot_container, CONTENT_Y, "Premium",
                           &lbl_copilot_accept_pct, &_pill, &bar_copilot, &lbl_copilot_detail);
    }

    // Panel 2 — remaining count + reset info
    {
        lv_obj_t *_pill, *_bar;
        make_copilot_panel(copilot_container, CONTENT_Y + COPILOT_PANEL_H + COPILOT_PANEL_GAP, "Quota",
                           &lbl_copilot_suggest, &_pill, &_bar, &lbl_copilot_status);
        lv_label_set_text(lbl_copilot_suggest, "---");
    }

    // Pixel-art Copilot mascot animation (60x60, aligned bottom-centre)
    splash_copilot_init(copilot_container);

    lv_obj_add_flag(copilot_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== Sysinfo Screen (135x240) ========
// Three 60px panels: CPU, RAM, Disk. Reuses COPILOT_PANEL_H/GAP constants.

static void init_sysinfo_screen(lv_obj_t* scr) {
    sysinfo_container = make_screen_container(scr, "System");

    int y = CONTENT_Y;

    // Panel 1 — CPU
    {
        lv_obj_t *_pill;
        make_copilot_panel(sysinfo_container, y, "CPU",
                           &lbl_cpu_pct, &_pill, &bar_cpu, &lbl_cpu_detail);
        lv_label_set_text(lbl_cpu_pct, "---%");
    }
    y += COPILOT_PANEL_H + COPILOT_PANEL_GAP;

    // Panel 2 — RAM
    {
        lv_obj_t *_pill;
        make_copilot_panel(sysinfo_container, y, "RAM",
                           &lbl_ram_pct, &_pill, &bar_ram, &lbl_ram_detail);
        lv_label_set_text(lbl_ram_pct, "---%");
    }
    y += COPILOT_PANEL_H + COPILOT_PANEL_GAP;

    // Panel 3 — Disk
    {
        lv_obj_t *_pill;
        make_copilot_panel(sysinfo_container, y, "Disk",
                           &lbl_disk_pct, &_pill, &bar_disk, &lbl_disk_detail);
        lv_label_set_text(lbl_disk_pct, "---%");
    }

    lv_obj_add_flag(sysinfo_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== VS Code Screen (135x240) ========
// Three 60px panels: Memory, Extensions, Errors. Reuses COPILOT_PANEL_H/GAP constants.

static void init_vscode_screen(lv_obj_t* scr) {
    vscode_container = make_screen_container(scr, "VS Code");

    int y = CONTENT_Y;

    // Panel 1 — Memory (MB)
    {
        lv_obj_t* _pill;
        make_copilot_panel(vscode_container, y, "Memory",
                           &lbl_vscode_mem, &_pill, &bar_vscode_mem, &lbl_vscode_mem_detail);
        lv_label_set_text(lbl_vscode_mem, "---");
    }
    y += COPILOT_PANEL_H + COPILOT_PANEL_GAP;

    // Panel 2 — Extensions
    {
        lv_obj_t* _pill;
        make_copilot_panel(vscode_container, y, "Ext",
                           &lbl_vscode_ext, &_pill, &bar_vscode_ext, &lbl_vscode_ext_detail);
        lv_label_set_text(lbl_vscode_ext, "---");
    }
    y += COPILOT_PANEL_H + COPILOT_PANEL_GAP;

    // Panel 3 — Errors
    {
        lv_obj_t* _pill;
        make_copilot_panel(vscode_container, y, "Errors",
                           &lbl_vscode_err, &_pill, &bar_vscode_err, &lbl_vscode_err_detail);
        lv_label_set_text(lbl_vscode_err, "---");
    }

    lv_obj_add_flag(vscode_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== Clock / Weather Screen (135x240) ========

static const char* weather_desc(int c) {
    switch (c) {
        case 0:  return "Clear";
        case 1:  return "Mainly clear";
        case 2:  return "Partly cloudy";
        case 3:  return "Overcast";
        case 45: case 48: return "Fog";
        case 51: case 53: case 55: case 56: case 57: return "Drizzle";
        case 61: case 63: case 65: case 66: case 67: return "Rain";
        case 71: case 73: case 75: case 77:          return "Snow";
        case 80: case 81: case 82:                   return "Showers";
        case 85: case 86:                            return "Snow showers";
        case 95: case 96: case 99:                   return "Thunderstorm";
        default: return "--";
    }
}

static lv_color_t weather_color(int c) {
    switch (c) {
        case 0: case 1:                              return COL_AMBER;             // sun
        case 71: case 73: case 75: case 77:
        case 85: case 86:                            return COL_TEXT;             // snow
        case 95: case 96: case 99:                   return COL_RED;              // storm
        default:
            if (c >= 51 && c <= 82) return lv_color_hex(0x5b8cb4);                // rain
            return COL_DIM;                                                       // cloud / fog
    }
}

// ---- Focus timer ----
static uint32_t banner_auto_hide_ms = 0;   // non-zero → hide the banner at this millis

static void timer_start_phase(tmr_state_t phase) {
    g_tmr = phase;
    tmr_end_ms = millis() + (phase == TMR_FOCUS ? TMR_FOCUS_MS : TMR_BREAK_MS);
    if (lbl_banner) {
        lv_label_set_text(lbl_banner, phase == TMR_FOCUS ? "Focus" : "Break time");
        banner_set(true);
        banner_auto_hide_ms = millis() + 4000;   // transient — unlike "needs you"
    }
    ui_flash_feedback_strong();
}

void ui_timer_toggle(void) {
    if (g_tmr == TMR_OFF) {
        tmr_round = 1;
        timer_start_phase(TMR_FOCUS);
    } else {
        g_tmr = TMR_OFF;
        banner_auto_hide_ms = 0;
        ui_hide_banner();
        refresh_clock(true);
        refresh_usage_strip();
    }
}

// Called from the 1 Hz clock tick while on the Clock screen. Owns the clock
// labels + one strip bar while a timer is running.
static void timer_tick(void) {
    if (banner_auto_hide_ms && millis() > banner_auto_hide_ms) {
        banner_auto_hide_ms = 0;
        ui_hide_banner();
    }
    if (g_tmr == TMR_OFF) return;
    long rem = (long)tmr_end_ms - (long)millis();
    if (rem <= 0) {
        if (g_tmr == TMR_FOCUS) { timer_start_phase(TMR_BREAK); }
        else                    { tmr_round++; timer_start_phase(TMR_FOCUS); }
        rem = (long)tmr_end_ms - (long)millis();
    }
    int total = (g_tmr == TMR_FOCUS ? TMR_FOCUS_MS : TMR_BREAK_MS);
    int s = rem / 1000;
    char b[16];
    snprintf(b, sizeof(b), "%02d:%02d", s / 60, s % 60);
    lv_label_set_text(lbl_clock_time, b);
    if (g_tmr == TMR_FOCUS) lv_label_set_text_fmt(lbl_clock_date, "FOCUS %d", tmr_round);
    else                    lv_label_set_text(lbl_clock_date, "BREAK");

    int elapsed_pct = 100 - (int)(100L * rem / total);
    lv_bar_set_value(bar_strip_claude, elapsed_pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_strip_claude,
        g_tmr == TMR_FOCUS ? COL_ACCENT : COL_GREEN, LV_PART_INDICATOR);
}

// Local time from the last synced epoch + elapsed millis. Only redraws when the
// minute changes unless `force`.
static void refresh_clock(bool force) {
    if (!lbl_clock_time) return;
    if (g_tmr != TMR_OFF) return;   // focus timer owns the labels
    if (!env_time_valid) {
        if (force) {
            lv_label_set_text(lbl_clock_time, "--:--");
            lv_label_set_text(lbl_clock_date, "waiting for time");
        }
        return;
    }
    long secs = env_epoch
              + (long)((millis() - env_rx_millis) / 1000UL)
              + (long)env_tz_off_min * 60;
    time_t t = (time_t)secs;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    if (!force && tmv.tm_min == last_shown_min) return;
    last_shown_min = tmv.tm_min;

    char b[24];
    snprintf(b, sizeof(b), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    lv_label_set_text(lbl_clock_time, b);

    static const char* const wd[7] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
    static const char* const mo[12] = {"JAN","FEB","MAR","APR","MAY","JUN",
                                       "JUL","AUG","SEP","OCT","NOV","DEC"};
    snprintf(b, sizeof(b), "%s %d %s",
             wd[tmv.tm_wday % 7], tmv.tm_mday, mo[tmv.tm_mon % 12]);
    lv_label_set_text(lbl_clock_date, b);
}

// Bottom "overall usage" strip: Claude session % + Copilot premium %.
static void refresh_usage_strip(void) {
    if (!bar_strip_claude) return;
    if (g_session_pct >= 0) {
        lv_bar_set_value(bar_strip_claude, g_session_pct, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar_strip_claude, pct_color((float)g_session_pct), LV_PART_INDICATOR);
    } else {
        lv_bar_set_value(bar_strip_claude, 0, LV_ANIM_OFF);
    }
    if (g_premium_pct >= 0) {
        lv_bar_set_value(bar_strip_copilot, g_premium_pct, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar_strip_copilot, pct_color((float)g_premium_pct), LV_PART_INDICATOR);
    } else {
        lv_bar_set_value(bar_strip_copilot, 0, LV_ANIM_OFF);
    }
}

static void init_env_screen(lv_obj_t* scr) {
    clock_container = make_screen_container(scr, "Clock");

    lbl_clock_time = lv_label_create(clock_container);
    lv_label_set_text(lbl_clock_time, "--:--");
    lv_obj_set_style_text_font(lbl_clock_time, &font_tiempos_34, 0);
    lv_obj_set_style_text_color(lbl_clock_time, COL_TEXT, 0);
    lv_obj_align(lbl_clock_time, LV_ALIGN_TOP_MID, 0, 38);

    lbl_clock_date = lv_label_create(clock_container);
    lv_label_set_text(lbl_clock_date, "");
    lv_obj_set_style_text_font(lbl_clock_date, &font_styrene_16, 0);
    lv_obj_set_style_text_color(lbl_clock_date, COL_DIM, 0);
    lv_obj_align(lbl_clock_date, LV_ALIGN_TOP_MID, 0, 78);

    lv_obj_t* rule = lv_obj_create(clock_container);
    lv_obj_set_size(rule, CONTENT_W - 24, 2);
    lv_obj_align(rule, LV_ALIGN_TOP_MID, 0, 106);
    lv_obj_set_style_bg_color(rule, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rule, 0, 0);
    lv_obj_set_style_radius(rule, 0, 0);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_SCROLLABLE);

    wx_icon = lv_obj_create(clock_container);
    lv_obj_set_size(wx_icon, 22, 22);
    lv_obj_set_style_radius(wx_icon, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(wx_icon, 0, 0);
    lv_obj_set_style_bg_color(wx_icon, COL_DIM, 0);
    lv_obj_set_style_bg_opa(wx_icon, LV_OPA_COVER, 0);
    lv_obj_clear_flag(wx_icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(wx_icon, MARGIN + 2, 116);

    lbl_wx_temp = lv_label_create(clock_container);
    lv_label_set_text(lbl_wx_temp, "--");
    lv_obj_set_style_text_font(lbl_wx_temp, &font_styrene_24, 0);
    lv_obj_set_style_text_color(lbl_wx_temp, COL_TEXT, 0);
    lv_obj_set_pos(lbl_wx_temp, MARGIN + 34, 112);

    // Degree mark — a small ring (no ° glyph in the ASCII-only fonts).
    lbl_wx_deg = lv_obj_create(clock_container);
    lv_obj_set_size(lbl_wx_deg, 7, 7);
    lv_obj_set_style_radius(lbl_wx_deg, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(lbl_wx_deg, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(lbl_wx_deg, 2, 0);
    lv_obj_set_style_border_color(lbl_wx_deg, COL_DIM, 0);
    lv_obj_set_style_pad_all(lbl_wx_deg, 0, 0);
    lv_obj_clear_flag(lbl_wx_deg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align_to(lbl_wx_deg, lbl_wx_temp, LV_ALIGN_OUT_RIGHT_TOP, 3, 3);

    // Indoor temp (env sensor) — same line / same size, right-aligned, dimmed.
    lbl_clock_intemp = lv_label_create(clock_container);
    lv_label_set_text(lbl_clock_intemp, "--");
    lv_obj_set_style_text_font(lbl_clock_intemp, &font_styrene_24, 0);
    lv_obj_set_style_text_color(lbl_clock_intemp, COL_DIM, 0);
    lv_obj_align(lbl_clock_intemp, LV_ALIGN_TOP_RIGHT, -MARGIN - 10, 112);

    lbl_in_deg = lv_obj_create(clock_container);
    lv_obj_set_size(lbl_in_deg, 7, 7);
    lv_obj_set_style_radius(lbl_in_deg, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(lbl_in_deg, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(lbl_in_deg, 2, 0);
    lv_obj_set_style_border_color(lbl_in_deg, COL_DIM, 0);
    lv_obj_set_style_pad_all(lbl_in_deg, 0, 0);
    lv_obj_clear_flag(lbl_in_deg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align_to(lbl_in_deg, lbl_clock_intemp, LV_ALIGN_OUT_RIGHT_TOP, 3, 3);
    lv_obj_add_flag(lbl_in_deg, LV_OBJ_FLAG_HIDDEN);

    lbl_wx_cond = lv_label_create(clock_container);
    lv_label_set_text(lbl_wx_cond, "");
    lv_obj_set_style_text_font(lbl_wx_cond, &font_styrene_14, 0);
    lv_obj_set_style_text_color(lbl_wx_cond, COL_TEXT, 0);
    lv_obj_align(lbl_wx_cond, LV_ALIGN_TOP_MID, 0, 142);

    lbl_wx_hilo = lv_label_create(clock_container);
    lv_label_set_text(lbl_wx_hilo, "");
    lv_obj_set_style_text_font(lbl_wx_hilo, &font_styrene_12, 0);
    lv_obj_set_style_text_color(lbl_wx_hilo, COL_DIM, 0);
    lv_obj_align(lbl_wx_hilo, LV_ALIGN_TOP_MID, 0, 160);

    lbl_wx_loc = lv_label_create(clock_container);
    lv_label_set_text(lbl_wx_loc, "");
    lv_obj_set_style_text_font(lbl_wx_loc, &font_styrene_12, 0);
    lv_obj_set_style_text_color(lbl_wx_loc, COL_DIM, 0);
    lv_obj_align(lbl_wx_loc, LV_ALIGN_TOP_MID, 0, 174);

    // Indoor reading (env sensor, on-device) — pressure (+humidity) line.
    lbl_clock_indoor = lv_label_create(clock_container);
    lv_label_set_text(lbl_clock_indoor, "-- hPa");
    lv_obj_set_style_text_font(lbl_clock_indoor, &font_styrene_12, 0);
    lv_obj_set_style_text_color(lbl_clock_indoor, COL_DIM, 0);
    lv_obj_align(lbl_clock_indoor, LV_ALIGN_TOP_MID, 0, 188);

    // Bottom overall-usage strip
    lv_obj_t* lbl_cl = lv_label_create(clock_container);
    lv_label_set_text(lbl_cl, "CL");
    lv_obj_set_style_text_font(lbl_cl, &font_styrene_12, 0);
    lv_obj_set_style_text_color(lbl_cl, COL_DIM, 0);
    lv_obj_set_pos(lbl_cl, MARGIN, 203);
    bar_strip_claude = make_bar(clock_container, MARGIN + 24, 207, CONTENT_W - 24, 6);

    lv_obj_t* lbl_cp = lv_label_create(clock_container);
    lv_label_set_text(lbl_cp, "CP");
    lv_obj_set_style_text_font(lbl_cp, &font_styrene_12, 0);
    lv_obj_set_style_text_color(lbl_cp, COL_DIM, 0);
    lv_obj_set_pos(lbl_cp, MARGIN, 216);
    bar_strip_copilot = make_bar(clock_container, MARGIN + 24, 220, CONTENT_W - 24, 6);

    lv_obj_add_flag(clock_container, LV_OBJ_FLAG_HIDDEN);
}

static void init_bluetooth_screen(lv_obj_t* scr) {
    ble_container = make_screen_container(scr, "Bluetooth");

    // Info panel
    lv_obj_t* p_info = make_panel(ble_container, MARGIN, CONTENT_Y, CONTENT_W, 108);

    // Bluetooth icon (centered at top of panel)
    static lv_image_dsc_t icon_bt_dsc;
    init_icon_dsc(&icon_bt_dsc, ICON_BLUETOOTH_W, ICON_BLUETOOTH_H, icon_bluetooth_data);

    lv_obj_t* bt_img = lv_image_create(p_info);
    lv_image_set_src(bt_img, &icon_bt_dsc);
    lv_obj_set_pos(bt_img, (CONTENT_W - 16 - ICON_BLUETOOTH_W) / 2, 0);

    lbl_ble_status = lv_label_create(p_info);
    lv_label_set_text(lbl_ble_status, "Initializing...");
    lv_obj_set_style_text_font(lbl_ble_status, &font_styrene_14, 0);
    lv_obj_set_style_text_color(lbl_ble_status, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_status, 0, 52);

    lbl_ble_device = lv_label_create(p_info);
    lv_label_set_text(lbl_ble_device, "---");
    lv_obj_set_style_text_font(lbl_ble_device, &font_styrene_12, 0);
    lv_obj_set_style_text_color(lbl_ble_device, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_device, 0, 70);

    lbl_ble_mac = lv_label_create(p_info);
    lv_label_set_text(lbl_ble_mac, "---");
    lv_obj_set_style_text_font(lbl_ble_mac, &font_styrene_12, 0);
    lv_obj_set_style_text_color(lbl_ble_mac, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_mac, 0, 86);

    // Unpair hint — the action is a long-press of the physical button while
    // this screen is showing (see main.cpp). No touch on this board.
    int reset_y = CONTENT_Y + 108 + 8;
    lv_obj_t* reset_zone = lv_obj_create(ble_container);
    lv_obj_set_pos(reset_zone, MARGIN, reset_y);
    lv_obj_set_size(reset_zone, CONTENT_W, 38);
    lv_obj_set_style_bg_color(reset_zone, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(reset_zone, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(reset_zone, 8, 0);
    lv_obj_set_style_border_width(reset_zone, 0, 0);
    lv_obj_set_flex_flow(reset_zone, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(reset_zone, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(reset_zone, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* reset_lbl = lv_label_create(reset_zone);
    lv_label_set_text(reset_lbl, "Hold button to unpair");
    lv_obj_set_style_text_font(reset_lbl, &font_styrene_12, 0);
    lv_obj_set_style_text_color(reset_lbl, COL_DIM, 0);

    // Attribution
    lv_obj_t* lbl_credit = lv_label_create(ble_container);
    lv_label_set_text(lbl_credit, "@hermannbjorgvin");
    lv_obj_set_style_text_font(lbl_credit, &font_styrene_12, 0);
    lv_obj_set_style_text_color(lbl_credit, COL_DIM, 0);
    lv_obj_align(lbl_credit, LV_ALIGN_BOTTOM_MID, 0, -22);

    lv_obj_t* lbl_credit2 = lv_label_create(ble_container);
    lv_label_set_text(lbl_credit2, "@amaanbuilds");
    lv_obj_set_style_text_font(lbl_credit2, &font_styrene_12, 0);
    lv_obj_set_style_text_color(lbl_credit2, COL_DIM, 0);
    lv_obj_align(lbl_credit2, LV_ALIGN_BOTTOM_MID, 0, -4);

    // Start hidden
    lv_obj_add_flag(ble_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== Checks Screen (CI + review queue + git) ========

static lv_obj_t* ci_container;
static lv_obj_t* ci_dot;
static lv_obj_t* lbl_ci_state;
static lv_obj_t* lbl_ci_wf;
static lv_obj_t* lbl_ci_pr;
static lv_obj_t* lbl_ci_git;
static bool ci_has_data = false;

static void init_ci_screen(lv_obj_t* scr) {
    ci_container = make_screen_container(scr, "Checks");

    ci_dot = lv_obj_create(ci_container);
    lv_obj_set_size(ci_dot, 24, 24);
    lv_obj_set_style_radius(ci_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ci_dot, 0, 0);
    lv_obj_set_style_bg_color(ci_dot, COL_DIM, 0);
    lv_obj_set_style_bg_opa(ci_dot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(ci_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(ci_dot, MARGIN + 4, CONTENT_Y + 4);

    lbl_ci_state = lv_label_create(ci_container);
    lv_label_set_text(lbl_ci_state, "No runs");
    lv_obj_set_style_text_font(lbl_ci_state, &font_styrene_24, 0);
    lv_obj_set_style_text_color(lbl_ci_state, COL_TEXT, 0);
    lv_obj_set_pos(lbl_ci_state, MARGIN + 38, CONTENT_Y);

    lbl_ci_wf = lv_label_create(ci_container);
    lv_label_set_text(lbl_ci_wf, "");
    lv_obj_set_style_text_font(lbl_ci_wf, &font_styrene_12, 0);
    lv_obj_set_style_text_color(lbl_ci_wf, COL_DIM, 0);
    lv_obj_set_pos(lbl_ci_wf, MARGIN + 38, CONTENT_Y + 26);

    lv_obj_t* rule = lv_obj_create(ci_container);
    lv_obj_set_size(rule, CONTENT_W - 24, 2);
    lv_obj_align(rule, LV_ALIGN_TOP_MID, 0, CONTENT_Y + 52);
    lv_obj_set_style_bg_color(rule, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rule, 0, 0);
    lv_obj_set_style_radius(rule, 0, 0);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_SCROLLABLE);

    lbl_ci_pr = lv_label_create(ci_container);
    lv_label_set_text(lbl_ci_pr, "");
    lv_obj_set_style_text_font(lbl_ci_pr, &font_styrene_14, 0);
    lv_obj_set_style_text_color(lbl_ci_pr, COL_TEXT, 0);
    lv_obj_set_pos(lbl_ci_pr, MARGIN, CONTENT_Y + 66);

    lbl_ci_git = lv_label_create(ci_container);
    lv_label_set_text(lbl_ci_git, "");
    lv_obj_set_style_text_font(lbl_ci_git, &font_styrene_12, 0);
    lv_obj_set_style_text_color(lbl_ci_git, COL_DIM, 0);
    lv_obj_set_pos(lbl_ci_git, MARGIN, CONTENT_Y + 92);

    lv_obj_add_flag(ci_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== Today Screen (daily summary) ========

static lv_obj_t* today_container;
static lv_obj_t* lbl_today_rows[5];   // Claude time / tokens / cost / commits / copilot
static bool today_has_data = false;

static lv_obj_t* sensor_container;
static lv_obj_t* lbl_sensor_temp_v;
static lv_obj_t* lbl_sensor_hum_label;    // "Humidity" — hidden when the active chip lacks it (BMP180)
static lv_obj_t* lbl_sensor_hum_v;
static lv_obj_t* lbl_sensor_press_label;  // repositioned depending on whether Humidity is shown
static lv_obj_t* lbl_sensor_press_v;
static bool sensor_has_data = false;
static bool sensor_shows_humidity = false;  // current layout state — reposition only when this changes

static lv_obj_t* make_today_row(lv_obj_t* parent, int y, const char* label) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, &font_styrene_12, 0);
    lv_obj_set_style_text_color(l, COL_DIM, 0);
    lv_obj_set_pos(l, MARGIN + 2, y);
    lv_obj_t* v = lv_label_create(parent);
    lv_label_set_text(v, "--");
    lv_obj_set_style_text_font(v, &font_styrene_16, 0);
    lv_obj_set_style_text_color(v, COL_TEXT, 0);
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -MARGIN - 2, y - 3);
    return v;
}

static void init_today_screen(lv_obj_t* scr) {
    today_container = make_screen_container(scr, "Today");
    int y = CONTENT_Y + 6;
    const char* names[5] = {"Claude", "Tokens", "Cost", "Commits", "Copilot"};
    for (int i = 0; i < 5; i++) {
        lbl_today_rows[i] = make_today_row(today_container, y, names[i]);
        y += 32;
    }
    lv_obj_add_flag(today_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== Sensor Screen (env sensor: BME280 or BMP180, on-device I2C) ========

#define SENSOR_ROW0_Y (CONTENT_Y + 6)

// Like make_today_row(), but also hands back the label object — the
// Humidity row needs to hide/show as a pair (label + value) depending on
// which chip is active.
static lv_obj_t* make_sensor_row(lv_obj_t* parent, int y, const char* label,
                                  lv_obj_t** out_label) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, &font_styrene_12, 0);
    lv_obj_set_style_text_color(l, COL_DIM, 0);
    lv_obj_set_pos(l, MARGIN + 2, y);
    lv_obj_t* v = lv_label_create(parent);
    lv_label_set_text(v, "--");
    lv_obj_set_style_text_font(v, &font_styrene_16, 0);
    lv_obj_set_style_text_color(v, COL_TEXT, 0);
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -MARGIN - 2, y - 3);
    if (out_label) *out_label = l;
    return v;
}

static void init_sensor_screen(lv_obj_t* scr) {
    sensor_container = make_screen_container(scr, "Sensor");
    lbl_sensor_temp_v  = make_sensor_row(sensor_container, SENSOR_ROW0_Y, "Temp", nullptr);
    lbl_sensor_hum_v   = make_sensor_row(sensor_container, SENSOR_ROW0_Y + 32, "Humidity", &lbl_sensor_hum_label);
    lbl_sensor_press_v = make_sensor_row(sensor_container, SENSOR_ROW0_Y + 64, "Press", &lbl_sensor_press_label);
    lv_obj_add_flag(sensor_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== Public API ========

void ui_init(void) {
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Logo (shared, always visible, on top of all containers)
    // Logo is RGB565A8 (planar: w*h RGB565 then w*h alpha) so it composites
    // cleanly against whatever bg is behind it.
    init_icon_dsc_rgb565a8(&logo_dsc, LOGO_WIDTH, LOGO_HEIGHT, logo_data);

    // Initialize battery icon descriptors
    init_battery_icons();

    init_env_screen(scr);
    init_usage_screen(scr);
    init_copilot_screen(scr);
    init_sysinfo_screen(scr);
    init_vscode_screen(scr);
    init_ci_screen(scr);
    init_today_screen(scr);
    init_sensor_screen(scr);
    init_bluetooth_screen(scr);
    splash_init(scr);

    // Logo: 80x80 is too large for 135px screen — hidden
    logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, &logo_dsc);
    lv_obj_set_pos(logo_img, MARGIN, TITLE_Y - 10);
    lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);

    // Battery indicator: hidden when no PMU present (pct always -1)
    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    lv_obj_set_pos(battery_img, SCR_W - 48 - MARGIN, TITLE_Y);
    lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);

    // Data-freshness indicator (shared, top-right). Updated once/sec by
    // ui_tick_anim() → refresh_status_label(); hidden on the splash screen.
    lbl_status_corner = lv_label_create(scr);
    lv_label_set_text(lbl_status_corner, "");
    lv_obj_set_style_text_font(lbl_status_corner, &font_styrene_12, 0);
    lv_obj_set_style_text_color(lbl_status_corner, COL_DIM, 0);
    lv_obj_align(lbl_status_corner, LV_ALIGN_TOP_RIGHT, -MARGIN, TITLE_Y + 2);

    // Claude-activity glyph — a small dot just left of the freshness pill,
    // colour = state (grey idle / accent working / red needs-you). Positioned
    // and shown/hidden alongside the pill in refresh_status_label().
    act_dot = lv_obj_create(scr);
    lv_obj_set_size(act_dot, 8, 8);
    lv_obj_set_style_radius(act_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(act_dot, 0, 0);
    lv_obj_set_style_bg_color(act_dot, COL_DIM, 0);
    lv_obj_set_style_bg_opa(act_dot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(act_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(act_dot, LV_OBJ_FLAG_HIDDEN);

    // "Claude needs you" banner overlay — shown on ACT_NEEDS_INPUT, dismissed
    // by any short button press (see main.cpp). Below the flash overlay.
    banner = lv_obj_create(scr);
    lv_obj_set_size(banner, SCR_W, 42);
    lv_obj_align(banner, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(banner, COL_ACCENT, 0);
    lv_obj_set_style_bg_opa(banner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(banner, 0, 0);
    lv_obj_set_style_radius(banner, 0, 0);
    lv_obj_set_style_pad_all(banner, 0, 0);
    lv_obj_clear_flag(banner, LV_OBJ_FLAG_SCROLLABLE);
    lbl_banner = lv_label_create(banner);
    lv_label_set_text(lbl_banner, "Claude needs you");
    lv_obj_set_style_text_font(lbl_banner, &font_styrene_14, 0);
    lv_obj_set_style_text_color(lbl_banner, COL_BG, 0);
    lv_obj_center(lbl_banner);
    lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);

    // Button-press flash overlay: fullscreen, non-clickable, topmost.
    // Starts fully transparent; ui_flash_feedback() pulses it briefly to
    // confirm a press was registered (no haptics on this board).
    flash_overlay = lv_obj_create(scr);
    lv_obj_set_size(flash_overlay, SCR_W, SCR_H);
    lv_obj_set_pos(flash_overlay, 0, 0);
    lv_obj_set_style_bg_color(flash_overlay, COL_ACCENT, 0);
    lv_obj_set_style_bg_opa(flash_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(flash_overlay, 0, 0);
    lv_obj_set_style_radius(flash_overlay, 0, 0);
    lv_obj_clear_flag(flash_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(flash_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(flash_overlay);
}

static void note_data(void) {
    last_data_ms = lv_tick_get();
    ever_data = true;
}

void ui_update_daemon_state(const char* state) {
    strlcpy(daemon_state, state ? state : "", sizeof(daemon_state));
    // A status frame is still a sign of life from the daemon.
    last_data_ms = lv_tick_get();
    ever_data = true;
    refresh_status_label();
}

bool ui_banner_visible(void) {
    return banner && !lv_obj_has_flag(banner, LV_OBJ_FLAG_HIDDEN);
}

void ui_hide_banner(void) {
    if (banner) lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
}

bool ui_claude_working(void) { return g_act == ACT_WORKING; }

static void banner_set(bool show) {
    if (!banner) return;
    if (show) {
        lv_obj_clear_flag(banner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(banner);
        if (flash_overlay) lv_obj_move_foreground(flash_overlay);
    } else {
        lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_update_act(const char* state, int agents) {
    act_state_t prev = g_act;
    act_state_t s = ACT_IDLE;
    if      (strcmp(state, "working") == 0)     s = ACT_WORKING;
    else if (strcmp(state, "needs_input") == 0) s = ACT_NEEDS_INPUT;
    else if (strcmp(state, "done") == 0)        s = ACT_DONE;
    else if (strcmp(state, "idle") == 0)        s = ACT_IDLE;

    g_act = s;
    g_act_agents = agents;
    note_data();

    if (s == ACT_NEEDS_INPUT && prev != ACT_NEEDS_INPUT) {
        lv_label_set_text(lbl_banner, "Claude needs you");
        banner_auto_hide_ms = 0;   // persistent until dismissed, unlike the timer
        banner_set(true);
        ui_flash_feedback_strong();
    } else if (s != ACT_NEEDS_INPUT && prev == ACT_NEEDS_INPUT) {
        banner_set(false);
    }

    // Drive the splash mood from the real signal: idle→sleepy, working→"work"
    // group, done→"active", needs-input→"surprise" group.
    int grp = (s == ACT_WORKING) ? 1
            : (s == ACT_DONE || s == ACT_NEEDS_INPUT) ? 2
            : 0;
    splash_set_activity(grp);

    refresh_status_label();
}

void ui_update(const UsageData* data) {
    if (!data->valid) return;
    note_data();
    daemon_state[0] = '\0';  // a fresh usage payload means the Claude poll is OK

    int s_pct = (int)(data->session_pct + 0.5f);
    g_session_pct = s_pct;
    g_session_reset_mins = data->session_reset_mins;
    refresh_usage_strip();

    // Usage screen
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

    // Model + context-window usage line under the title.
    if (data->model[0] && data->ctx_pct >= 0) {
        lv_label_set_text_fmt(lbl_model, "%s - ctx %d%%", data->model, data->ctx_pct);
        lv_obj_set_style_text_color(lbl_model,
            data->ctx_pct >= 85 ? COL_RED : data->ctx_pct >= 70 ? COL_AMBER : COL_DIM, 0);
    } else if (data->model[0]) {
        lv_label_set_text(lbl_model, data->model);
        lv_obj_set_style_text_color(lbl_model, COL_DIM, 0);
    } else {
        lv_label_set_text(lbl_model, "");
    }
}

void ui_update_copilot(const CopilotData* data) {
    if (!data->valid) return;
    note_data();

    g_premium_pct = (data->premium_pct >= 0) ? data->premium_pct : -1;
    refresh_usage_strip();

    // Panel 1 — premium request usage %
    if (data->premium_pct >= 0) {
        lv_label_set_text_fmt(lbl_copilot_accept_pct, "%d%%", data->premium_pct);
        lv_bar_set_value(bar_copilot, data->premium_pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(bar_copilot, pct_color((float)data->premium_pct), LV_PART_INDICATOR);
    } else {
        lv_label_set_text(lbl_copilot_accept_pct, "---%");
        lv_bar_set_value(bar_copilot, 0, LV_ANIM_OFF);
    }

    char buf[48];
    if (data->premium_remaining >= 0 && data->premium_total > 0) {
        snprintf(buf, sizeof(buf), "%d / %d left", data->premium_remaining, data->premium_total);
    } else {
        snprintf(buf, sizeof(buf), "No quota data");
    }
    lv_label_set_text(lbl_copilot_detail, buf);

    // Panel 2 — remaining count + reset countdown
    if (data->premium_remaining >= 0) {
        lv_label_set_text_fmt(lbl_copilot_suggest, "%d", data->premium_remaining);
    } else {
        lv_label_set_text(lbl_copilot_suggest, "---");
    }
    char reset_buf[12] = "---";
    if (data->premium_reset_mins > 0) {
        int days = data->premium_reset_mins / 1440;
        if (days > 0) snprintf(reset_buf, sizeof(reset_buf), "%dd", days);
        else          snprintf(reset_buf, sizeof(reset_buf), "%dh", data->premium_reset_mins / 60);
    }
    // styrene_12 is ASCII-only — no middle dot / degree glyphs.
    snprintf(buf, sizeof(buf), "%.12s - resets %s", data->plan, reset_buf);
    lv_label_set_text(lbl_copilot_status, buf);
}

void ui_update_sysinfo(const SysInfoData* data) {
    if (!data->valid) return;
    note_data();
    sysinfo_has_data = true;

    char buf[40];

    // CPU panel
    if (data->cpu_pct >= 0) {
        lv_label_set_text_fmt(lbl_cpu_pct, "%d%%", data->cpu_pct);
        lv_bar_set_value(bar_cpu, data->cpu_pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(bar_cpu, pct_color((float)data->cpu_pct), LV_PART_INDICATOR);
    } else {
        lv_label_set_text(lbl_cpu_pct, "---%");
        lv_bar_set_value(bar_cpu, 0, LV_ANIM_OFF);
    }
    if (data->cpu_temp >= 0.0f) {
        snprintf(buf, sizeof(buf), "%.0f C", (double)data->cpu_temp);
    } else {
        snprintf(buf, sizeof(buf), "---");
    }
    lv_label_set_text(lbl_cpu_detail, buf);

    // RAM panel
    if (data->ram_pct >= 0) {
        lv_label_set_text_fmt(lbl_ram_pct, "%d%%", data->ram_pct);
        lv_bar_set_value(bar_ram, data->ram_pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(bar_ram, pct_color((float)data->ram_pct), LV_PART_INDICATOR);
        snprintf(buf, sizeof(buf), "%.1f / %.1f GB",
                 (double)data->ram_used_gb, (double)data->ram_total_gb);
    } else {
        lv_label_set_text(lbl_ram_pct, "---%");
        lv_bar_set_value(bar_ram, 0, LV_ANIM_OFF);
        snprintf(buf, sizeof(buf), "---");
    }
    lv_label_set_text(lbl_ram_detail, buf);

    // Disk panel
    if (data->disk_pct >= 0) {
        lv_label_set_text_fmt(lbl_disk_pct, "%d%%", data->disk_pct);
        lv_bar_set_value(bar_disk, data->disk_pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(bar_disk, pct_color((float)data->disk_pct), LV_PART_INDICATOR);
        snprintf(buf, sizeof(buf), "%.0f / %.0f GB",
                 (double)data->disk_used_gb, (double)data->disk_total_gb);
    } else {
        lv_label_set_text(lbl_disk_pct, "---%");
        lv_bar_set_value(bar_disk, 0, LV_ANIM_OFF);
        snprintf(buf, sizeof(buf), "---");
    }
    lv_label_set_text(lbl_disk_detail, buf);
}

void ui_update_vscode(const VscodeData* data) {
    if (!data->valid) return;
    note_data();
    vscode_has_data = true;

    char buf[40];

    // Memory panel — "1.2G" / "840M" (keep it short next to the pill), bar = pct of 4096MB cap.
    // NB: lv_label_set_text_fmt has no %f — use integer math for the decimal.
    if (data->mem_mb >= 0) {
        if (data->mem_mb >= 1000) {
            int g10 = (data->mem_mb + 50) / 100;  // GB × 10, rounded
            lv_label_set_text_fmt(lbl_vscode_mem, "%d.%dG", g10 / 10, g10 % 10);
        } else {
            lv_label_set_text_fmt(lbl_vscode_mem, "%dM", data->mem_mb);
        }
        int mem_pct = (data->mem_mb * 100) / 4096;
        if (mem_pct > 100) mem_pct = 100;
        lv_bar_set_value(bar_vscode_mem, mem_pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(bar_vscode_mem, pct_color((float)mem_pct), LV_PART_INDICATOR);
        if (data->cpu_pct >= 0)
            snprintf(buf, sizeof(buf), "CPU %d%%", data->cpu_pct);
        else
            snprintf(buf, sizeof(buf), "---");
    } else {
        lv_label_set_text(lbl_vscode_mem, "---");
        lv_bar_set_value(bar_vscode_mem, 0, LV_ANIM_OFF);
        snprintf(buf, sizeof(buf), "---");
    }
    lv_label_set_text(lbl_vscode_mem_detail, buf);

    // Extensions panel — count, bar = ext_count/50 capped
    if (data->ext_count >= 0) {
        lv_label_set_text_fmt(lbl_vscode_ext, "%d", data->ext_count);
        int ext_pct = (data->ext_count * 100) / 50;
        if (ext_pct > 100) ext_pct = 100;
        lv_bar_set_value(bar_vscode_ext, ext_pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(bar_vscode_ext, COL_GREEN, LV_PART_INDICATOR);
        snprintf(buf, sizeof(buf), "ext hosts");
    } else {
        lv_label_set_text(lbl_vscode_ext, "---");
        lv_bar_set_value(bar_vscode_ext, 0, LV_ANIM_OFF);
        snprintf(buf, sizeof(buf), "---");
    }
    lv_label_set_text(lbl_vscode_ext_detail, buf);

    // Errors panel — count, bar = errors/10 capped, red if any
    if (data->error_count >= 0) {
        lv_label_set_text_fmt(lbl_vscode_err, "%d", data->error_count);
        int err_pct = (data->error_count * 100) / 10;
        if (err_pct > 100) err_pct = 100;
        lv_bar_set_value(bar_vscode_err, err_pct > 0 ? err_pct : 1, LV_ANIM_ON);
        lv_color_t err_col = (data->error_count > 0) ? COL_RED : COL_GREEN;
        lv_obj_set_style_bg_color(bar_vscode_err, err_col, LV_PART_INDICATOR);
        if (data->error_count == 0)
            snprintf(buf, sizeof(buf), "no errors");
        else
            snprintf(buf, sizeof(buf), "%.28s", data->last_error);
    } else {
        lv_label_set_text(lbl_vscode_err, "---");
        lv_bar_set_value(bar_vscode_err, 0, LV_ANIM_OFF);
        snprintf(buf, sizeof(buf), "---");
    }
    lv_label_set_text(lbl_vscode_err_detail, buf);
}

void ui_update_env(const EnvData* data) {
    if (!data->valid) return;
    note_data();

    if (data->epoch > 0) {
        env_epoch      = data->epoch;
        env_rx_millis  = millis();
        env_tz_off_min = data->tz_off_min;
        env_time_valid = true;
        last_shown_min = -1;
        refresh_clock(true);
    }

    if (data->has_weather && data->wcode >= 0) {
        lv_label_set_text_fmt(lbl_wx_temp, "%d", data->temp_c);
        lv_obj_align_to(lbl_wx_deg, lbl_wx_temp, LV_ALIGN_OUT_RIGHT_TOP, 3, 5);
        lv_obj_clear_flag(lbl_wx_deg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(wx_icon, weather_color(data->wcode), 0);
        lv_label_set_text(lbl_wx_cond, weather_desc(data->wcode));
        lv_label_set_text_fmt(lbl_wx_hilo, "H %d    L %d", data->hi_c, data->lo_c);
    } else {
        lv_label_set_text(lbl_wx_temp, "--");
        lv_obj_add_flag(lbl_wx_deg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(wx_icon, COL_BAR_BG, 0);
        lv_label_set_text(lbl_wx_cond, "weather --");
        lv_label_set_text(lbl_wx_hilo, "");
    }
    lv_label_set_text(lbl_wx_loc, data->loc);
}

void ui_update_ci(const CiData* data) {
    if (!data->valid) return;
    note_data();
    ci_has_data = true;

    lv_color_t dc; const char* st;
    if      (strcmp(data->state, "pass") == 0)    { dc = COL_GREEN; st = "Passing"; }
    else if (strcmp(data->state, "fail") == 0)    { dc = COL_RED;   st = "Failing"; }
    else if (strcmp(data->state, "running") == 0) { dc = COL_AMBER; st = "Running"; }
    else                                          { dc = COL_DIM;   st = "No runs"; }
    lv_obj_set_style_bg_color(ci_dot, dc, 0);
    lv_label_set_text(lbl_ci_state, st);

    char b[48];
    if (data->wf[0] && data->age_min >= 0) {
        if (data->age_min < 60) snprintf(b, sizeof(b), "%.14s - %dm ago", data->wf, data->age_min);
        else                    snprintf(b, sizeof(b), "%.14s - %dh ago", data->wf, data->age_min / 60);
    } else if (data->wf[0]) {
        snprintf(b, sizeof(b), "%.16s", data->wf);
    } else {
        b[0] = '\0';
    }
    lv_label_set_text(lbl_ci_wf, b);

    if (data->review > 0 && data->changes > 0)
        snprintf(b, sizeof(b), "%d review  %d chg", data->review, data->changes);
    else if (data->review > 0)
        snprintf(b, sizeof(b), "%d to review", data->review);
    else if (data->changes > 0)
        snprintf(b, sizeof(b), "%d need work", data->changes);
    else
        snprintf(b, sizeof(b), "queue clear");
    lv_label_set_text(lbl_ci_pr, b);
    lv_obj_set_style_text_color(lbl_ci_pr, data->changes > 0 ? COL_AMBER : COL_TEXT, 0);

    // git working tree: "main  4 changed  +2 -1  (conflict)"
    int p = snprintf(b, sizeof(b), "%.20s", data->branch[0] ? data->branch : "-");
    if (data->dirty > 0)  p += snprintf(b + p, sizeof(b) - p, "  %d chg", data->dirty);
    if (data->ahead || data->behind)
        p += snprintf(b + p, sizeof(b) - p, "  +%d -%d", data->ahead, data->behind);
    if (data->conflict)   snprintf(b + p, sizeof(b) - p, "  !conflict");
    lv_label_set_text(lbl_ci_git, b);
}

void ui_update_today(int act_min, int tok_k, int usd, int commits, int cp_used) {
    note_data();
    today_has_data = true;
    char b[16];

    if (act_min >= 60) snprintf(b, sizeof(b), "%dh %02dm", act_min / 60, act_min % 60);
    else               snprintf(b, sizeof(b), "%dm", act_min);
    lv_label_set_text(lbl_today_rows[0], b);

    if (tok_k >= 1000) snprintf(b, sizeof(b), "%d.%dM", tok_k / 1000, (tok_k % 1000) / 100);
    else               snprintf(b, sizeof(b), "%dk", tok_k);
    lv_label_set_text(lbl_today_rows[1], b);

    if (usd > 0) snprintf(b, sizeof(b), "$%d", usd);
    else         snprintf(b, sizeof(b), "--");
    lv_label_set_text(lbl_today_rows[2], b);

    snprintf(b, sizeof(b), "%d", commits);
    lv_label_set_text(lbl_today_rows[3], b);

    if (cp_used >= 0) snprintf(b, sizeof(b), "%d", cp_used);
    else              snprintf(b, sizeof(b), "--");
    lv_label_set_text(lbl_today_rows[4], b);
}

// Env sensor reading (BME280 or BMP180, whichever env_sensor.cpp found),
// pushed locally by main.cpp — not a daemon payload, so no note_data() here;
// this shouldn't count toward daemon-link freshness.
void ui_update_sensor(bool present, float temp_c, float pressure_hpa,
                       bool has_humidity, float humidity_pct) {
    sensor_has_data = present;
    if (!present) {
        if (lbl_clock_intemp) {
            lv_label_set_text(lbl_clock_intemp, "--");
            lv_obj_add_flag(lbl_in_deg, LV_OBJ_FLAG_HIDDEN);
        }
        if (lbl_clock_indoor) lv_label_set_text(lbl_clock_indoor, "-- hPa");
        return;
    }

    if (has_humidity != sensor_shows_humidity) {
        sensor_shows_humidity = has_humidity;
        if (has_humidity) {
            lv_obj_clear_flag(lbl_sensor_hum_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lbl_sensor_hum_v, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(lbl_sensor_press_label, MARGIN + 2, SENSOR_ROW0_Y + 64);
            lv_obj_align(lbl_sensor_press_v, LV_ALIGN_TOP_RIGHT, -MARGIN - 2, SENSOR_ROW0_Y + 64 - 3);
        } else {
            lv_obj_add_flag(lbl_sensor_hum_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lbl_sensor_hum_v, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(lbl_sensor_press_label, MARGIN + 2, SENSOR_ROW0_Y + 32);
            lv_obj_align(lbl_sensor_press_v, LV_ALIGN_TOP_RIGHT, -MARGIN - 2, SENSOR_ROW0_Y + 32 - 3);
        }
    }

    char b[24];
    snprintf(b, sizeof(b), "%.1f C", (double)temp_c);
    lv_label_set_text(lbl_sensor_temp_v, b);
    if (has_humidity) {
        snprintf(b, sizeof(b), "%.0f%%", (double)humidity_pct);
        lv_label_set_text(lbl_sensor_hum_v, b);
    }
    snprintf(b, sizeof(b), "%.0fhPa", (double)pressure_hpa);
    lv_label_set_text(lbl_sensor_press_v, b);

    if (lbl_clock_intemp) {
        snprintf(b, sizeof(b), "%.0f", (double)temp_c);
        lv_label_set_text(lbl_clock_intemp, b);
        lv_obj_align_to(lbl_in_deg, lbl_clock_intemp, LV_ALIGN_OUT_RIGHT_TOP, 3, 5);
        lv_obj_clear_flag(lbl_in_deg, LV_OBJ_FLAG_HIDDEN);
    }

    if (lbl_clock_indoor) {
        if (has_humidity) snprintf(b, sizeof(b), "%.0f hPa   %.0f%%", (double)pressure_hpa, (double)humidity_pct);
        else              snprintf(b, sizeof(b), "%.0f hPa", (double)pressure_hpa);
        lv_label_set_text(lbl_clock_indoor, b);
    }
}

// "safe to run" / "cap ~1h40m" / "cap imminent" — from the smoothed session
// %/min and the reset countdown. false when there's no usable rate yet.
static bool quota_verdict(char* buf, size_t n, lv_color_t* col) {
    float rate = usage_rate_per_min();
    if (rate <= 0.02f || g_session_pct < 0) return false;
    float mins = (100.0f - g_session_pct) / rate;
    if (mins < 0) mins = 0;
    int m = (int)(mins + 0.5f);
    if (g_session_reset_mins > 0 && mins > g_session_reset_mins) {
        snprintf(buf, n, "safe to run");
        *col = COL_GREEN;
    } else if (m <= 20) {
        snprintf(buf, n, "cap imminent");
        *col = COL_RED;
    } else if (m < 60) {
        snprintf(buf, n, "cap ~%dm", m);
        *col = COL_AMBER;
    } else {
        snprintf(buf, n, "cap ~%dh%02dm", m / 60, m % 60);
        *col = COL_AMBER;
    }
    return true;
}

// True when the daemon link is healthy: connected, has sent data, recently,
// and not reporting an error state.
static bool data_is_live(void) {
    return ble_get_state() == BLE_STATE_CONNECTED
        && ever_data
        && (lv_tick_get() - last_data_ms) < STALE_MS
        && (daemon_state[0] == '\0' || strcmp(daemon_state, "ok") == 0);
}

// Top-right freshness pill: "12s" / "4m" (green→amber by age), "stale",
// "offline", "waiting", or a daemon error ("no token"). Hidden on splash.
static void refresh_status_label(void) {
    if (!lbl_status_corner) return;
    // Splash has no header; the Bluetooth screen shows connection state in full
    // already (and "Bluetooth" is wide enough to crowd the pill).
    if (current_screen == SCREEN_SPLASH || current_screen == SCREEN_BLUETOOTH) {
        lv_obj_add_flag(lbl_status_corner, LV_OBJ_FLAG_HIDDEN);
        if (act_dot) lv_obj_add_flag(act_dot, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(lbl_status_corner, LV_OBJ_FLAG_HIDDEN);

    // Activity glyph colour/visibility (positioned after the pill, below).
    bool show_dot = act_dot && g_act != ACT_UNKNOWN;
    if (act_dot) {
        if (!show_dot) {
            lv_obj_add_flag(act_dot, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_color_t dc = (g_act == ACT_NEEDS_INPUT) ? COL_RED
                          : (g_act == ACT_WORKING)     ? COL_ACCENT
                          : (g_act == ACT_DONE)        ? COL_GREEN
                          :                              COL_DIM;
            lv_obj_set_style_bg_color(act_dot, dc, 0);
            lv_obj_clear_flag(act_dot, LV_OBJ_FLAG_HIDDEN);
        }
    }

    const char* txt;
    lv_color_t  col;
    uint32_t    age = lv_tick_get() - last_data_ms;

    if (ble_get_state() != BLE_STATE_CONNECTED) {
        txt = "offline";
        col = COL_RED;
    } else if (daemon_state[0] != '\0' && strcmp(daemon_state, "ok") != 0) {
        if      (strcmp(daemon_state, "no_token") == 0)  txt = "no token";
        else if (strcmp(daemon_state, "api_error") == 0) txt = "API error";
        else                                            txt = daemon_state;
        col = COL_RED;
    } else if (!ever_data) {
        txt = "waiting";
        col = COL_DIM;
    } else if (age >= VERY_STALE_MS) {
        txt = "stale";
        col = COL_RED;
    } else {
        static char b[16];
        if (age < 60000UL)        snprintf(b, sizeof(b), "%lus", (unsigned long)(age / 1000UL));
        else if (age < 3600000UL) snprintf(b, sizeof(b), "%lum", (unsigned long)(age / 60000UL));
        else                      snprintf(b, sizeof(b), "%luh", (unsigned long)(age / 3600000UL));
        txt = b;
        col = (age >= STALE_MS) ? COL_AMBER : COL_GREEN;
    }
    lv_label_set_text(lbl_status_corner, txt);
    lv_obj_set_style_text_color(lbl_status_corner, col, 0);
    lv_obj_align(lbl_status_corner, LV_ALIGN_TOP_RIGHT, -MARGIN, TITLE_Y + 2);
    if (show_dot) {
        lv_obj_update_layout(lbl_status_corner);
        lv_obj_align_to(act_dot, lbl_status_corner, LV_ALIGN_OUT_LEFT_MID, -5, 0);
    }
}

void ui_tick_anim(void) {
    uint32_t now = lv_tick_get();

    // Freshness pill + clock — refresh roughly once per second, on every screen.
    static uint32_t status_last_ms = 0;
    if (now - status_last_ms >= 1000) {
        status_last_ms = now;
        refresh_status_label();
        timer_tick();  // runs on any screen so the break banner still fires
        if (current_screen == SCREEN_CLOCK) refresh_clock(false);
    }

    // Copilot screen: advance pixel-art mascot animation
    if (current_screen == SCREEN_COPILOT) {
        splash_copilot_tick();
        return;
    }

    if (current_screen != SCREEN_USAGE) return;

    // When the daemon link is not live, the "working" spinner would imply
    // activity that isn't happening — show a static status line instead.
    if (!data_is_live()) {
        lv_obj_set_style_text_color(lbl_anim, COL_DIM, 0);
        lv_label_set_text(lbl_anim,
            ble_get_state() == BLE_STATE_CONNECTED ? "waiting\xE2\x80\xA6"
                                                   : "offline");
        return;
    }

    // Run the gerund spinner only when Claude is actually working (or when
    // we have no activity signal at all — keeps the old behaviour). Otherwise
    // the bottom line carries the "safe to run?" quota verdict.
    if (g_act != ACT_WORKING && g_act != ACT_UNKNOWN) {
        static uint32_t verdict_ms = 0;
        if (now - verdict_ms >= 2000) {
            verdict_ms = now;
            char b[24];
            lv_color_t c;
            if (quota_verdict(b, sizeof(b), &c)) {
                lv_obj_set_style_text_color(lbl_anim, c, 0);
                lv_label_set_text(lbl_anim, b);
            } else {
                lv_obj_set_style_text_color(lbl_anim, COL_DIM, 0);
                lv_label_set_text(lbl_anim, g_act == ACT_NEEDS_INPUT ? "needs you" : "idle");
            }
        }
        return;
    }

    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);

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

static screen_t prev_non_splash_screen = SCREEN_CLOCK;
// Hide the battery indicator on the splash screen — the icon is visually
// noisy over the pixel-art creature animations.
// On other screens, ui_update_battery() controls visibility; don't unconditionally
// unhide here (no PMU on this board, so it should stay hidden).
static void apply_battery_visibility(void) {
    if (!battery_img) return;
    if (current_screen == SCREEN_SPLASH) lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
}

void ui_show_screen(screen_t screen) {
    lv_obj_add_flag(clock_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(copilot_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(sysinfo_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(vscode_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ci_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(today_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(sensor_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ble_container, LV_OBJ_FLAG_HIDDEN);
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:     splash_show(); break;
    case SCREEN_CLOCK:
        lv_obj_clear_flag(clock_container, LV_OBJ_FLAG_HIDDEN);
        refresh_clock(true);
        refresh_usage_strip();
        break;
    case SCREEN_USAGE:      lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_COPILOT:    lv_obj_clear_flag(copilot_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_SYSINFO:    lv_obj_clear_flag(sysinfo_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_VSCODE:     lv_obj_clear_flag(vscode_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_CI:         lv_obj_clear_flag(ci_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_TODAY:      lv_obj_clear_flag(today_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_SENSOR:     lv_obj_clear_flag(sensor_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_BLUETOOTH:  lv_obj_clear_flag(ble_container, LV_OBJ_FLAG_HIDDEN); break;
    default: break;
    }

    // Logo is 80×80 — too large for 135px screen; keep permanently hidden.
    if (logo_img) lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
    apply_battery_visibility();
    refresh_status_label();
}

void ui_cycle_screen(void) {
    screen_t next = current_screen;
    do {
        if (next == SCREEN_CLOCK)          next = SCREEN_SENSOR;
        else if (next == SCREEN_SENSOR)    next = SCREEN_USAGE;
        else if (next == SCREEN_USAGE)     next = SCREEN_COPILOT;
        else if (next == SCREEN_COPILOT)   next = SCREEN_SYSINFO;
        else if (next == SCREEN_SYSINFO)   next = SCREEN_VSCODE;
        else if (next == SCREEN_VSCODE)    next = SCREEN_BLUETOOTH;
        else if (next == SCREEN_BLUETOOTH) next = SCREEN_CI;
        else if (next == SCREEN_CI)        next = SCREEN_TODAY;
        else if (next == SCREEN_TODAY)     next = SCREEN_SPLASH;
        else                              next = SCREEN_CLOCK;  // from SPLASH (or first run)
        // Skip screens that have never received data from the daemon so
        // cycling only surfaces screens with real content.
        if (next == SCREEN_SYSINFO && !sysinfo_has_data) continue;
        if (next == SCREEN_VSCODE && !vscode_has_data) continue;
        if (next == SCREEN_CI && !ci_has_data) continue;
        if (next == SCREEN_TODAY && !today_has_data) continue;
        if (next == SCREEN_SENSOR && !sensor_has_data) continue;
        break;
    } while (true);
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

    // Raw name/MAC — no "Device:"/"Address:" prefix; a 123px panel can't hold it.
    if (name) lv_label_set_text(lbl_ble_device, name);
    if (mac)  lv_label_set_text(lbl_ble_mac, mac);
}

void ui_update_battery(int percent, bool charging) {
    // No PMU on this board — hide battery indicator
    if (!charging && percent < 0) {
        if (battery_img) lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    int idx;
    if (charging) {
        idx = 4;  // charging icon
    } else if (percent <= 10) {
        idx = 0;  // empty
    } else if (percent <= 35) {
        idx = 1;  // low
    } else if (percent <= 75) {
        idx = 2;  // medium
    } else {
        idx = 3;  // full
    }
    lv_image_set_src(battery_img, &battery_dscs[idx]);
    apply_battery_visibility();
}

// ---- Button-press flash overlay ----
// Brief brand-accent pulse on the top layer to confirm a physical button
// press was registered (no haptics on this board). Non-clickable so it
// never intercepts taps meant for the screen underneath.
static void flash_anim_cb(void* obj, int32_t v) {
    lv_obj_set_style_bg_opa((lv_obj_t*)obj, (lv_opa_t)v, 0);
}

static void flash_pulse(lv_opa_t peak, uint32_t ms) {
    if (!flash_overlay) return;
    lv_obj_set_style_bg_opa(flash_overlay, peak, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, flash_overlay);
    lv_anim_set_exec_cb(&a, flash_anim_cb);
    lv_anim_set_values(&a, peak, 0);
    lv_anim_set_duration(&a, ms);
    lv_anim_start(&a);
}

// Light pulse — short press (screen advance).
void ui_flash_feedback(void) { flash_pulse(70, 160); }

// Firmer, longer pulse — long press / committed action (refresh, unpair).
void ui_flash_feedback_strong(void) { flash_pulse(150, 320); }
