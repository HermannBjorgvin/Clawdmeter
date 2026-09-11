#include "ui.h"
#include "splash.h"
#include <lvgl.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include "ble.h"
#include "usage_rate.h"
#include "logo.h"
#include "icons.h"
#include "display_cfg.h"
#include "sensor_hist.h"
#include "usage_hist.h"

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
static lv_obj_t* lbl_wx_out;
static lv_obj_t* lbl_wx_in;
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
static lv_obj_t*   lbl_agent_badge = NULL;  // "×N" — shown when >1 agent is running

// ---- Cross-screen alert banner (shared, on scr) ----
// One widget, several possible occupants. Priority is a flat ranking, not a
// queue: a higher (or equal) priority kind always wins the widget, and
// clearing only takes effect if that kind is the one currently shown — so a
// lower-priority alert that got pre-empted is simply dropped, not restored,
// when the one that pre-empted it clears. Acceptable for how rare it is for
// two of these to be live at once; revisit if that stops being true.
enum banner_kind_t { BANNER_NONE = 0, BANNER_TIMER = 1, BANNER_ALERT = 2, BANNER_NEEDS_YOU = 3 };
static banner_kind_t g_banner_kind = BANNER_NONE;
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

// One-shot "near your limit" banner alert for session/weekly usage — fires
// once per crossing into the danger zone, clears once back under it.
#define USAGE_ALERT_PCT 90

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
static void banner_show(banner_kind_t kind, const char* text, lv_color_t bg,
                        lv_color_t fg, uint32_t auto_hide_ms);
static void banner_clear(banner_kind_t kind);

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
// 24 h micro-sparkline for one usage metric — no axes, no labels, just the
// trace. Tucked into the free space to the right of the reset-time label
// (see the width budget note by its caller); rebuilt on every payload via
// usage_spark_rebuild(), which is cheap enough to not need its own timer.
struct usage_spark_t {
    lv_obj_t*          line;
    lv_point_precise_t pts[UH_SLOTS];
    int16_t            w, h;
};
static usage_spark_t spark_session, spark_weekly;

#define USPARK_W  28
#define USPARK_H  12

static lv_obj_t* make_usage_spark(lv_obj_t* parent, lv_color_t col, usage_spark_t* out) {
    out->w = USPARK_W;
    out->h = USPARK_H;
    out->line = lv_line_create(parent);
    lv_obj_set_size(out->line, out->w, out->h);
    lv_obj_align(out->line, LV_ALIGN_TOP_RIGHT, 0, 44);
    lv_obj_set_style_pad_all(out->line, 0, 0);
    lv_obj_set_style_line_width(out->line, 2, 0);
    lv_obj_set_style_line_color(out->line, col, 0);
    lv_obj_set_style_line_rounded(out->line, true, 0);
    lv_obj_add_flag(out->line, LV_OBJ_FLAG_HIDDEN);   // shown once there's a trace
    return out->line;
}

// Mirrors trend_rebuild() in the sensor trend engine, minus the scale
// labels this compact a trace has no room for.
static void usage_spark_rebuild(usage_spark_t* p, uh_metric_t m) {
    float v[UH_SLOTS];
    usage_hist_series(m, v, UH_SLOTS);

    float lo = 0, hi = 0;
    int   n = 0;
    for (int i = 0; i < UH_SLOTS; i++) {
        if (isnan(v[i])) continue;
        if (!n || v[i] < lo) lo = v[i];
        if (!n || v[i] > hi) hi = v[i];
        n++;
    }
    if (n < 2) {
        lv_obj_add_flag(p->line, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    float span = hi - lo;
    if (span < 3.0f) {   // flat-ish trace: park it mid-height rather than a hairline
        float mid = (hi + lo) / 2.0f;
        span = 3.0f;
        lo = mid - span / 2.0f;
        hi = mid + span / 2.0f;
    }

    const int32_t usable = p->h - 3;
    int k = 0;
    for (int i = 0; i < UH_SLOTS; i++) {
        if (isnan(v[i])) continue;
        p->pts[k].x = (lv_value_precise_t)((int32_t)i * (p->w - 1) / (UH_SLOTS - 1));
        p->pts[k].y = (lv_value_precise_t)(1 + usable -
                          (int32_t)lroundf((v[i] - lo) / span * (float)usable));
        k++;
    }
    lv_line_set_points(p->line, p->pts, k);
    lv_obj_clear_flag(p->line, LV_OBJ_FLAG_HIDDEN);
}

static void make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                             lv_color_t spark_col,
                             lv_obj_t** out_pct, lv_obj_t** out_pill,
                             lv_obj_t** out_bar, lv_obj_t** out_reset,
                             usage_spark_t* out_spark) {
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

    // 24 h trend, right of the reset label. Reset strings top out around
    // "Resets 6d 23h" (~13 chars, ~80px at this font) leaving a consistent
    // ~35px gap before the panel's right padding — the sparkline fits inside
    // that with room to spare. Deliberately not the green/amber/red status
    // palette (the bar above already carries that signal) — a neutral trace
    // color so the two aren't read as disagreeing.
    make_usage_spark(panel, spark_col, out_spark);
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

    // Sparkline colors are deliberately not the green/amber/red status
    // palette (see make_usage_panel) — just two distinct neutral hues.
    make_usage_panel(usage_container, CONTENT_Y, "Current", COL_ACCENT,
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset, &spark_session);
    make_usage_panel(usage_container, CONTENT_Y + PANEL_H + PANEL_GAP, "Weekly",
                     lv_color_hex(0x6a9ec0),
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset, &spark_weekly);

    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, &font_mono_18, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    // The longest gerunds ("Philosophising…", "Flibbertigibbeting…") overrun
    // 135px at this font — clip to the content width with an ellipsis rather
    // than letting them run off-screen.
    lv_obj_set_width(lbl_anim, CONTENT_W);
    lv_label_set_long_mode(lbl_anim, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(lbl_anim, LV_TEXT_ALIGN_CENTER, 0);
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
    banner_show(BANNER_TIMER, phase == TMR_FOCUS ? "Focus" : "Break time",
                COL_ACCENT, COL_BG, 4000);   // transient — unlike "needs you"
    ui_flash_feedback_strong();
}

void ui_timer_toggle(void) {
    if (g_tmr == TMR_OFF) {
        tmr_round = 1;
        timer_start_phase(TMR_FOCUS);
    } else {
        g_tmr = TMR_OFF;
        banner_clear(BANNER_TIMER);
        refresh_clock(true);
        refresh_usage_strip();
    }
}

// Called from the 1 Hz clock tick while on the Clock screen. Owns the clock
// labels + one strip bar while a timer is running.
static void timer_tick(void) {
    if (banner_auto_hide_ms && millis() > banner_auto_hide_ms) {
        banner_auto_hide_ms = 0;
        banner_clear(g_banner_kind);   // clears whichever kind set the auto-hide
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

    lbl_wx_out = lv_label_create(clock_container);
    lv_label_set_text(lbl_wx_out, "OUT");
    lv_obj_set_style_text_font(lbl_wx_out, &font_styrene_12, 0);
    lv_obj_set_style_text_color(lbl_wx_out, COL_DIM, 0);
    lv_obj_set_pos(lbl_wx_out, MARGIN + 2, 108);

    lbl_wx_in = lv_label_create(clock_container);
    lv_label_set_text(lbl_wx_in, "IN");
    lv_obj_set_style_text_font(lbl_wx_in, &font_styrene_12, 0);
    lv_obj_set_style_text_color(lbl_wx_in, COL_DIM, 0);
    lv_obj_align(lbl_wx_in, LV_ALIGN_TOP_RIGHT, -MARGIN, 108);

    wx_icon = lv_obj_create(clock_container);
    lv_obj_set_size(wx_icon, 22, 22);
    lv_obj_set_style_radius(wx_icon, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(wx_icon, 0, 0);
    lv_obj_set_style_bg_color(wx_icon, COL_DIM, 0);
    lv_obj_set_style_bg_opa(wx_icon, LV_OPA_COVER, 0);
    lv_obj_clear_flag(wx_icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(wx_icon, MARGIN + 2, 120);

    lbl_wx_temp = lv_label_create(clock_container);
    lv_label_set_text(lbl_wx_temp, "--");
    lv_obj_set_style_text_font(lbl_wx_temp, &font_styrene_24, 0);
    lv_obj_set_style_text_color(lbl_wx_temp, COL_TEXT, 0);
    lv_obj_set_pos(lbl_wx_temp, MARGIN + 34, 118);

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
    lv_obj_align(lbl_clock_intemp, LV_ALIGN_TOP_RIGHT, -MARGIN - 10, 118);

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
    lv_label_set_text(lbl_clock_indoor, "P -- hPa");
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
static lv_obj_t* dot_ci_branch;    // small marker before the branch name — a
                                    // visual anchor so the git row isn't just
                                    // a wall of abbreviated text
static lv_obj_t* lbl_ci_branch;
static lv_obj_t* pill_ci_dirty;    // "N chg", amber, hidden when clean
static lv_obj_t* pill_ci_ahead;    // "+N", green, hidden when 0
static lv_obj_t* pill_ci_behind;   // "-N", dim, hidden when 0
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

    // Git working-tree row: a small marker + branch name, then compact
    // status chips below — replaces one dense "main 4 chg +1 -0" line with
    // scannable, color-coded pieces (dirty=amber, ahead=green, behind=dim).
    dot_ci_branch = lv_obj_create(ci_container);
    lv_obj_set_size(dot_ci_branch, 6, 6);
    lv_obj_set_style_radius(dot_ci_branch, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot_ci_branch, 0, 0);
    lv_obj_set_style_bg_color(dot_ci_branch, COL_DIM, 0);
    lv_obj_set_style_bg_opa(dot_ci_branch, LV_OPA_COVER, 0);
    lv_obj_clear_flag(dot_ci_branch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(dot_ci_branch, MARGIN + 1, CONTENT_Y + 96);

    lbl_ci_branch = lv_label_create(ci_container);
    lv_label_set_text(lbl_ci_branch, "");
    lv_obj_set_style_text_font(lbl_ci_branch, &font_styrene_12, 0);
    lv_obj_set_style_text_color(lbl_ci_branch, COL_TEXT, 0);
    lv_obj_set_pos(lbl_ci_branch, MARGIN + 13, CONTENT_Y + 92);

    // Fixed 42px chip slots — a hidden chip just leaves a gap, which reads
    // fine for a status row (same idiom as CI check badges elsewhere).
    pill_ci_dirty = make_pill(ci_container, "");
    lv_obj_set_pos(pill_ci_dirty, MARGIN, CONTENT_Y + 112);
    lv_obj_add_flag(pill_ci_dirty, LV_OBJ_FLAG_HIDDEN);

    pill_ci_ahead = make_pill(ci_container, "");
    lv_obj_set_pos(pill_ci_ahead, MARGIN + 42, CONTENT_Y + 112);
    lv_obj_add_flag(pill_ci_ahead, LV_OBJ_FLAG_HIDDEN);

    pill_ci_behind = make_pill(ci_container, "");
    lv_obj_set_pos(pill_ci_behind, MARGIN + 84, CONTENT_Y + 112);
    lv_obj_add_flag(pill_ci_behind, LV_OBJ_FLAG_HIDDEN);

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
static bool sensor_shows_humidity = true;   // current layout state — reposition only when this changes;
                                            // seeded to match the as-built layout (humidity row present)

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

// ======== Sensor trend graphs (24 h, NVS-backed history) ========
//
// Feeds both the Sensor page's temperature sparkline and the dedicated
// "Trend 24h" page. LV_USE_CHART isn't compiled into this build, and an
// lv_line polyline over a pre-scaled point array does the same job for
// ~800 bytes of RAM per trace and no extra flash.

// Trace colours — deliberately NOT the status palette. Green/amber/red mean
// healthy/caution/critical everywhere else in this UI, and a room temperature
// is a measurement, not a health state.
#define COL_TR_TEMP   lv_color_hex(0xd97757)   // brand terra-cotta
#define COL_TR_HUM    lv_color_hex(0x6a9ec0)   // muted blue
#define COL_TR_PRESS  lv_color_hex(0xa89bc4)   // muted violet

// Smallest y-axis span we'll scale to, per metric. Without a floor, a room
// that held dead steady all day renders sensor noise as a mountain range.
static const float TREND_MIN_SPAN[SH_METRIC_COUNT] = {
    2.0f,   // degC
    5.0f,   // %RH
    4.0f,   // hPa
};

#define TREND_PAD     5     // card inset
#define TREND_GUTTER  34    // right-hand strip holding the hi/lo scale labels
#define TREND_HDR_H   22    // header row height -> top of the plot area

struct trend_plot_t {
    lv_obj_t*          line;
    lv_obj_t*          lbl_hi;    // scale top, aligned with the plot's top edge
    lv_obj_t*          lbl_lo;    // scale bottom
    lv_obj_t*          lbl_val;   // header right slot — caller decides what goes here
    lv_obj_t*          lbl_empty; // "collecting" placeholder while the trace is too short
    lv_point_precise_t pts[SH_SLOTS];
    int16_t            w, h;      // plot area in px
    sh_metric_t        metric;
};

static trend_plot_t spark;                    // Sensor page: temperature only
static trend_plot_t trend[SH_METRIC_COUNT];   // Trend 24h page: one per metric

// Live reading, cached by ui_update_sensor() so the trend page can show the
// current value in each panel header without re-reading the I2C bus.
static float g_env_t = NAN, g_env_h = NAN, g_env_p = NAN;

static void fmt_metric(sh_metric_t m, float v, char* b, size_t n, bool with_unit) {
    switch (m) {
    case SH_TEMP:  snprintf(b, n, with_unit ? "%.1f C"   : "%.1f", (double)v); break;
    case SH_HUM:   snprintf(b, n, with_unit ? "%.0f%%"   : "%.0f", (double)v); break;
    default:       snprintf(b, n, with_unit ? "%.0f hPa" : "%.0f", (double)v); break;
    }
}

// Move/resize a panel and the plot inside it. Only the height varies, and
// only lbl_lo tracks it — it's bottom-aligned, so LVGL re-places it for free.
static void trend_set_geom(lv_obj_t* card, trend_plot_t* p, int y, int h) {
    lv_obj_set_pos(card, MARGIN, y);
    lv_obj_set_size(card, CONTENT_W, h);
    p->h = (int16_t)(h - TREND_HDR_H - 4);
    lv_obj_set_size(p->line, p->w, p->h);
    lv_obj_align(p->lbl_empty, LV_ALIGN_TOP_MID,
                 -TREND_GUTTER / 2, TREND_HDR_H + p->h / 2 - 7);
}

static lv_obj_t* make_trend_panel(lv_obj_t* parent, int y, int h,
                                  const char* name, sh_metric_t m,
                                  lv_color_t col, trend_plot_t* out) {
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, CONTENT_W, h);
    lv_obj_set_pos(card, MARGIN, y);
    lv_obj_set_style_bg_color(card, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 6, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl = lv_label_create(card);
    lv_label_set_text(lbl, name);
    lv_obj_set_style_text_font(lbl, &font_styrene_12, 0);
    lv_obj_set_style_text_color(lbl, COL_DIM, 0);
    lv_obj_set_pos(lbl, TREND_PAD, 3);

    out->metric = m;
    out->w = (int16_t)(CONTENT_W - 2 * TREND_PAD - TREND_GUTTER);
    out->h = (int16_t)(h - TREND_HDR_H - 4);

    out->lbl_val = lv_label_create(card);
    lv_label_set_text(out->lbl_val, "--");
    lv_obj_set_style_text_font(out->lbl_val, &font_styrene_12, 0);
    lv_obj_set_style_text_color(out->lbl_val, col, 0);
    lv_obj_align(out->lbl_val, LV_ALIGN_TOP_RIGHT, -TREND_PAD, 3);

    out->line = lv_line_create(card);
    lv_obj_set_size(out->line, out->w, out->h);
    lv_obj_set_pos(out->line, TREND_PAD, TREND_HDR_H);
    lv_obj_set_style_pad_all(out->line, 0, 0);
    lv_obj_set_style_line_width(out->line, 2, 0);
    lv_obj_set_style_line_color(out->line, col, 0);
    lv_obj_set_style_line_rounded(out->line, true, 0);
    lv_obj_add_flag(out->line, LV_OBJ_FLAG_HIDDEN);

    out->lbl_hi = lv_label_create(card);
    lv_label_set_text(out->lbl_hi, "");
    lv_obj_set_style_text_font(out->lbl_hi, &font_styrene_12, 0);
    lv_obj_set_style_text_color(out->lbl_hi, COL_DIM, 0);
    lv_obj_align(out->lbl_hi, LV_ALIGN_TOP_RIGHT, -TREND_PAD, TREND_HDR_H - 4);

    out->lbl_lo = lv_label_create(card);
    lv_label_set_text(out->lbl_lo, "");
    lv_obj_set_style_text_font(out->lbl_lo, &font_styrene_12, 0);
    lv_obj_set_style_text_color(out->lbl_lo, COL_DIM, 0);
    lv_obj_align(out->lbl_lo, LV_ALIGN_BOTTOM_RIGHT, -TREND_PAD, -2);

    // A blank panel for the first 15 minutes after a wipe reads as broken
    // rather than as "no history yet" — say which it is.
    out->lbl_empty = lv_label_create(card);
    lv_label_set_text(out->lbl_empty, "collecting");
    lv_obj_set_style_text_font(out->lbl_empty, &font_styrene_12, 0);
    lv_obj_set_style_text_color(out->lbl_empty, COL_DIM, 0);
    lv_obj_add_flag(out->lbl_empty, LV_OBJ_FLAG_HIDDEN);

    trend_set_geom(card, out, y, h);   // positions the placeholder too
    return card;
}

// Rebuild one polyline from history. Hides the trace (and blanks the scale)
// until there are two samples to draw a line between.
static void trend_rebuild(trend_plot_t* p) {
    float v[SH_SLOTS];
    sensor_hist_series(p->metric, v, SH_SLOTS);

    float lo = 0, hi = 0;
    int   n = 0;
    for (int i = 0; i < SH_SLOTS; i++) {
        if (isnan(v[i])) continue;
        if (!n || v[i] < lo) lo = v[i];
        if (!n || v[i] > hi) hi = v[i];
        n++;
    }
    if (n < 2) {
        lv_obj_add_flag(p->line, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(p->lbl_empty, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(p->lbl_hi, "");
        lv_label_set_text(p->lbl_lo, "");
        return;
    }
    lv_obj_add_flag(p->lbl_empty, LV_OBJ_FLAG_HIDDEN);

    float span = hi - lo;
    if (span < TREND_MIN_SPAN[p->metric]) {   // park a flat trace mid-panel
        float mid = (hi + lo) / 2.0f;
        span = TREND_MIN_SPAN[p->metric];
        lo = mid - span / 2.0f;
        hi = mid + span / 2.0f;
    }

    // x is the sample's true position in the 24 h window, so a young history
    // draws a short trace hugging the right edge rather than stretching to
    // fill the panel. Gaps (the device was unplugged) bridge straight across.
    // Inset by a pixel top and bottom: the stroke is 2px wide, so a sample
    // sitting exactly on the min or max would have half its cap clipped off
    // by the plot bounds and read as a flat cut rather than a peak.
    const int32_t usable = p->h - 3;
    int k = 0;
    for (int i = 0; i < SH_SLOTS; i++) {
        if (isnan(v[i])) continue;
        p->pts[k].x = (lv_value_precise_t)((int32_t)i * (p->w - 1) / (SH_SLOTS - 1));
        p->pts[k].y = (lv_value_precise_t)(1 + usable -
                          (int32_t)lroundf((v[i] - lo) / span * (float)usable));
        k++;
    }
    lv_line_set_points(p->line, p->pts, k);
    lv_obj_clear_flag(p->line, LV_OBJ_FLAG_HIDDEN);

    char b[16];
    fmt_metric(p->metric, hi, b, sizeof(b), false); lv_label_set_text(p->lbl_hi, b);
    fmt_metric(p->metric, lo, b, sizeof(b), false); lv_label_set_text(p->lbl_lo, b);
}

// ---- Trend screen ----

static lv_obj_t* trend_container;
static lv_obj_t* trend_card_temp;
static lv_obj_t* trend_card_hum;    // hidden wholesale on a BMP180 (no humidity)
static lv_obj_t* trend_card_press;
static lv_obj_t* lbl_trend_from;    // x-axis left end — "-24h", or "~24h" while
                                    // the ring is still on a free-running clock

#define TREND_PANEL_H  62   // three metrics stacked
#define TREND_PANEL_H2 94   // two metrics — both grow into the spare slot
#define TREND_Y2_PRESS 128
static const int TREND_Y[SH_METRIC_COUNT] = { 30, 95, 160 };

static void init_trend_screen(lv_obj_t* scr) {
    // Just "Trend" — "Trend 24h" ran into the freshness pill, and the
    // window is spelled out by the axis labels at the foot of the page.
    trend_container = make_screen_container(scr, "Trend");

    trend_card_temp =
        make_trend_panel(trend_container, TREND_Y[SH_TEMP], TREND_PANEL_H,
                         "Temp", SH_TEMP, COL_TR_TEMP, &trend[SH_TEMP]);
    trend_card_hum =
        make_trend_panel(trend_container, TREND_Y[SH_HUM], TREND_PANEL_H,
                         "Humidity", SH_HUM, COL_TR_HUM, &trend[SH_HUM]);
    trend_card_press =
        make_trend_panel(trend_container, TREND_Y[SH_PRESS], TREND_PANEL_H,
                         "Pressure", SH_PRESS, COL_TR_PRESS, &trend[SH_PRESS]);

    lbl_trend_from = lv_label_create(trend_container);
    lv_label_set_text(lbl_trend_from, "-24h");
    lv_obj_set_style_text_font(lbl_trend_from, &font_styrene_12, 0);
    lv_obj_set_style_text_color(lbl_trend_from, COL_DIM, 0);
    lv_obj_set_pos(lbl_trend_from, MARGIN + TREND_PAD, 224);

    lv_obj_t* now = lv_label_create(trend_container);
    lv_label_set_text(now, "now");
    lv_obj_set_style_text_font(now, &font_styrene_12, 0);
    lv_obj_set_style_text_color(now, COL_DIM, 0);
    lv_obj_align(now, LV_ALIGN_TOP_RIGHT, -MARGIN - TREND_PAD - TREND_GUTTER, 224);

    lv_obj_add_flag(trend_container, LV_OBJ_FLAG_HIDDEN);
}

// Two panels when the active chip has no humidity channel (BMP180). Rather
// than leaving the third slot as dead space, the two survivors split it and
// get taller plots.
static void trend_apply_layout(bool has_humidity) {
    if (has_humidity) {
        lv_obj_clear_flag(trend_card_hum, LV_OBJ_FLAG_HIDDEN);
        trend_set_geom(trend_card_temp,  &trend[SH_TEMP],  TREND_Y[SH_TEMP],  TREND_PANEL_H);
        trend_set_geom(trend_card_hum,   &trend[SH_HUM],   TREND_Y[SH_HUM],   TREND_PANEL_H);
        trend_set_geom(trend_card_press, &trend[SH_PRESS], TREND_Y[SH_PRESS], TREND_PANEL_H);
    } else {
        lv_obj_add_flag(trend_card_hum, LV_OBJ_FLAG_HIDDEN);
        trend_set_geom(trend_card_temp,  &trend[SH_TEMP],  TREND_Y[SH_TEMP], TREND_PANEL_H2);
        trend_set_geom(trend_card_press, &trend[SH_PRESS], TREND_Y2_PRESS,   TREND_PANEL_H2);
    }
}

static void refresh_trend_screen(void) {
    // Until the daemon hands us a wall clock the ring can't account for time
    // spent powered down, so samples restored from NVS may sit later on the
    // axis than they belong. Say so rather than implying exact timing.
    lv_label_set_text(lbl_trend_from, sensor_hist_time_is_real() ? "-24h" : "~24h");

    const float live[SH_METRIC_COUNT] = { g_env_t, g_env_h, g_env_p };
    for (int m = 0; m < SH_METRIC_COUNT; m++) {
        if (m == SH_HUM && !sensor_shows_humidity) continue;
        trend_rebuild(&trend[m]);
        char b[16];
        if (isnan(live[m])) snprintf(b, sizeof(b), "--");
        else                fmt_metric((sh_metric_t)m, live[m], b, sizeof(b), true);
        lv_label_set_text(trend[m].lbl_val, b);
    }
}

// ---- Sensor page sparkline ----

// Sits under the reading rows and runs to the foot of the screen, so it grows
// by exactly the height of the humidity row the BMP180 doesn't need.
#define SPARK_BOTTOM  228
#define SPARK_Y_HUM   146   // below three reading rows (BME280)
#define SPARK_Y_NOHUM 114   // below two (BMP180 — no humidity row)

static lv_obj_t* spark_card;

static void init_sensor_spark(void) {
    spark_card = make_trend_panel(sensor_container, SPARK_Y_HUM,
                                  SPARK_BOTTOM - SPARK_Y_HUM,
                                  "Trend", SH_TEMP, COL_TR_TEMP, &spark);
}

static void spark_apply_layout(bool has_humidity) {
    int y = has_humidity ? SPARK_Y_HUM : SPARK_Y_NOHUM;
    trend_set_geom(spark_card, &spark, y, SPARK_BOTTOM - y);
}

// The header's right slot carries how much history we actually hold, which is
// what you want to know right after a reboot — the current temperature is
// already spelled out in the row above.
static void refresh_sensor_spark(void) {
    trend_rebuild(&spark);

    int mins = sensor_hist_span_mins();
    char b[16];
    if (mins <= 0)           snprintf(b, sizeof(b), "--");
    else if (mins < 60)      snprintf(b, sizeof(b), "%dm", mins);
    else if (mins % 60 == 0) snprintf(b, sizeof(b), "%dh", mins / 60);
    else                     snprintf(b, sizeof(b), "%dh%02d", mins / 60, mins % 60);
    lv_label_set_text(spark.lbl_val, b);
}

// Redraw whichever trend view is on screen. Cheap enough to call on every
// reading change, but rate-limited by ui_tick_anim() anyway.
static void refresh_trends_if_visible(void) {
    if (current_screen == SCREEN_SENSOR)            refresh_sensor_spark();
    else if (current_screen == SCREEN_SENSOR_GRAPH) refresh_trend_screen();
}

// ======== Public API ========

// ---- Screen-position indicator ----
// Single button, no touch, up to 10 populated screens in the cycle — a
// short row of dots gives a sense of "how many presses to get back here"
// without a persistent on-screen element. Shown for DOTS_VISIBLE_MS after
// every ui_show_screen(), then hidden by ui_tick_anim().
#define MAX_DOTS 10
static lv_obj_t* dot_objs[MAX_DOTS] = { nullptr };
static uint32_t  dots_hide_ms = 0;   // 0 = not pending
#define DOTS_VISIBLE_MS  1500

// Mirrors the traversal order in ui_cycle_screen() (SPLASH excluded — it's
// the "off ramp", not a stop with a position).
static const screen_t CYCLE_ORDER[] = {
    SCREEN_CLOCK, SCREEN_SENSOR, SCREEN_SENSOR_GRAPH, SCREEN_USAGE,
    SCREEN_COPILOT, SCREEN_SYSINFO, SCREEN_VSCODE, SCREEN_BLUETOOTH,
    SCREEN_CI, SCREEN_TODAY,
};
#define CYCLE_COUNT (sizeof(CYCLE_ORDER) / sizeof(CYCLE_ORDER[0]))

static bool screen_is_populated(screen_t s) {
    switch (s) {
    case SCREEN_SYSINFO:      return sysinfo_has_data;
    case SCREEN_VSCODE:       return vscode_has_data;
    case SCREEN_CI:           return ci_has_data;
    case SCREEN_TODAY:        return today_has_data;
    case SCREEN_SENSOR:
    case SCREEN_SENSOR_GRAPH: return sensor_has_data;
    default:                  return true;
    }
}

static void init_screen_dots(lv_obj_t* scr) {
    for (int i = 0; i < MAX_DOTS; i++) {
        lv_obj_t* d = lv_obj_create(scr);
        lv_obj_set_size(d, 4, 4);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(d, COL_DIM, 0);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
        dot_objs[i] = d;
    }
}

// Recompute + show the dot row for `screen`. No-op (fully hidden) on the
// splash screen or when the cycle currently holds just one stop.
static void refresh_screen_dots(screen_t screen) {
    int idx = -1, count = 0;
    if (screen != SCREEN_SPLASH) {
        for (unsigned i = 0; i < CYCLE_COUNT; i++) {
            if (!screen_is_populated(CYCLE_ORDER[i])) continue;
            if (CYCLE_ORDER[i] == screen) idx = count;
            count++;
        }
    }
    if (idx < 0 || count <= 1) {
        for (int i = 0; i < MAX_DOTS; i++) lv_obj_add_flag(dot_objs[i], LV_OBJ_FLAG_HIDDEN);
        dots_hide_ms = 0;
        return;
    }
    const int gap = 8;
    const int start_x = (SCR_W - (count - 1) * gap) / 2;
    for (int i = 0; i < MAX_DOTS; i++) {
        if (i >= count) { lv_obj_add_flag(dot_objs[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_clear_flag(dot_objs[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(dot_objs[i], start_x + i * gap - 2, 23);
        lv_obj_set_style_bg_color(dot_objs[i], (i == idx) ? COL_ACCENT : COL_DIM, 0);
        lv_obj_move_foreground(dot_objs[i]);
    }
    dots_hide_ms = lv_tick_get() + DOTS_VISIBLE_MS;
}

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
    init_sensor_spark();
    init_trend_screen(scr);
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

    // Agent-count badge — a small "×N" just left of the activity dot, shown
    // only when more than one agent is running (g_act_agents was tracked but
    // never surfaced anywhere before this).
    lbl_agent_badge = lv_label_create(scr);
    lv_label_set_text(lbl_agent_badge, "");
    lv_obj_set_style_text_font(lbl_agent_badge, &font_styrene_12, 0);
    lv_obj_set_style_text_color(lbl_agent_badge, COL_ACCENT, 0);
    lv_obj_add_flag(lbl_agent_badge, LV_OBJ_FLAG_HIDDEN);

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
    init_screen_dots(scr);

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

// User-initiated dismiss (any button press while the banner is up — see
// main.cpp). Unlike banner_clear(), this always wins: whatever is showing
// goes away, regardless of kind.
void ui_hide_banner(void) {
    g_banner_kind = BANNER_NONE;
    banner_auto_hide_ms = 0;
    if (banner) lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
}

bool ui_claude_working(void) { return g_act == ACT_WORKING; }

static void banner_show(banner_kind_t kind, const char* text, lv_color_t bg,
                        lv_color_t fg, uint32_t auto_hide_ms) {
    if (!banner || kind < g_banner_kind) return;   // something more important is up
    g_banner_kind = kind;
    lv_label_set_text(lbl_banner, text);
    lv_obj_set_style_bg_color(banner, bg, 0);
    lv_obj_set_style_text_color(lbl_banner, fg, 0);
    banner_auto_hide_ms = auto_hide_ms ? (millis() + auto_hide_ms) : 0;
    lv_obj_clear_flag(banner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(banner);
    if (flash_overlay) lv_obj_move_foreground(flash_overlay);
}

// Hide the banner, but only if `kind` is the one currently occupying it —
// an event going away (CI recovers, usage drops back down) shouldn't
// dismiss a different banner that has since taken over the widget.
static void banner_clear(banner_kind_t kind) {
    if (g_banner_kind != kind) return;
    g_banner_kind = BANNER_NONE;
    banner_auto_hide_ms = 0;
    if (banner) lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
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
        banner_show(BANNER_NEEDS_YOU, "Claude needs you", COL_ACCENT, COL_BG, 0);
        ui_flash_feedback_strong();
    } else if (s != ACT_NEEDS_INPUT && prev == ACT_NEEDS_INPUT) {
        banner_clear(BANNER_NEEDS_YOU);
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

    static bool session_alert_armed = false;
    if (s_pct >= USAGE_ALERT_PCT) {
        if (!session_alert_armed) {
            session_alert_armed = true;
            banner_show(BANNER_ALERT, "Session near limit", COL_RED, COL_TEXT, 0);
        }
    } else if (session_alert_armed) {
        session_alert_armed = false;
        banner_clear(BANNER_ALERT);
    }

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

    static bool weekly_alert_armed = false;
    if (w_pct >= USAGE_ALERT_PCT) {
        if (!weekly_alert_armed) {
            weekly_alert_armed = true;
            banner_show(BANNER_ALERT, "Weekly near limit", COL_RED, COL_TEXT, 0);
        }
    } else if (weekly_alert_armed) {
        weekly_alert_armed = false;
        banner_clear(BANNER_ALERT);
    }

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

    // 24 h sparklines — rebuilt on every payload, which is already the ring's
    // natural sampling cadence (usage_hist_sample() is fed from the same
    // payload in main.cpp), so there's no need for a separate refresh timer.
    usage_spark_rebuild(&spark_session, UH_SESSION);
    usage_spark_rebuild(&spark_weekly, UH_WEEKLY);
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
    bool failing = strcmp(data->state, "fail") == 0;
    if      (strcmp(data->state, "pass") == 0)    { dc = COL_GREEN; st = "Passing"; }
    else if (failing)                             { dc = COL_RED;   st = "Failing"; }
    else if (strcmp(data->state, "running") == 0) { dc = COL_AMBER; st = "Running"; }
    else                                          { dc = COL_DIM;   st = "No runs"; }
    lv_obj_set_style_bg_color(ci_dot, dc, 0);
    lv_label_set_text(lbl_ci_state, st);

    // Surface a fresh CI failure even if the Checks screen isn't the one on
    // screen right now; clears itself once CI recovers.
    static bool ci_alert_armed = false;
    if (failing && !ci_alert_armed) {
        ci_alert_armed = true;
        banner_show(BANNER_ALERT, "CI is failing", COL_RED, COL_TEXT, 0);
    } else if (!failing && ci_alert_armed) {
        ci_alert_armed = false;
        banner_clear(BANNER_ALERT);
    }

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

    // Git working tree: marker + branch name, then dirty/ahead/behind chips.
    // A conflict recolors the marker instead of adding a fourth chip — rare
    // enough not to need its own slot.
    snprintf(b, sizeof(b), "%.20s", data->branch[0] ? data->branch : "-");
    lv_label_set_text(lbl_ci_branch, b);
    lv_obj_set_style_bg_color(dot_ci_branch, data->conflict ? COL_RED : COL_DIM, 0);

    if (data->dirty > 0) {
        lv_label_set_text_fmt(pill_ci_dirty, "%d chg", data->dirty);
        lv_obj_set_style_bg_color(pill_ci_dirty, COL_AMBER, 0);
        lv_obj_set_style_text_color(pill_ci_dirty, COL_BG, 0);
        lv_obj_clear_flag(pill_ci_dirty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(pill_ci_dirty, LV_OBJ_FLAG_HIDDEN);
    }
    if (data->ahead > 0) {
        lv_label_set_text_fmt(pill_ci_ahead, "+%d", data->ahead);
        lv_obj_set_style_bg_color(pill_ci_ahead, COL_GREEN, 0);
        lv_obj_set_style_text_color(pill_ci_ahead, COL_BG, 0);
        lv_obj_clear_flag(pill_ci_ahead, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(pill_ci_ahead, LV_OBJ_FLAG_HIDDEN);
    }
    if (data->behind > 0) {
        lv_label_set_text_fmt(pill_ci_behind, "-%d", data->behind);
        lv_obj_set_style_bg_color(pill_ci_behind, COL_BAR_BG, 0);
        lv_obj_set_style_text_color(pill_ci_behind, COL_DIM, 0);
        lv_obj_clear_flag(pill_ci_behind, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(pill_ci_behind, LV_OBJ_FLAG_HIDDEN);
    }
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
        g_env_t = g_env_h = g_env_p = NAN;
        if (lbl_clock_intemp) {
            lv_label_set_text(lbl_clock_intemp, "--");
            lv_obj_add_flag(lbl_in_deg, LV_OBJ_FLAG_HIDDEN);
        }
        if (lbl_clock_indoor) lv_label_set_text(lbl_clock_indoor, "P -- hPa");
        return;
    }

    g_env_t = temp_c;
    g_env_p = pressure_hpa;
    g_env_h = has_humidity ? humidity_pct : NAN;

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
        spark_apply_layout(has_humidity);
        trend_apply_layout(has_humidity);
        // Panel heights just changed, so the cached y-scaling is stale.
        refresh_trends_if_visible();
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
        if (has_humidity) snprintf(b, sizeof(b), "P %.0fhPa  H %.0f%%", (double)pressure_hpa, (double)humidity_pct);
        else              snprintf(b, sizeof(b), "P %.0fhPa", (double)pressure_hpa);
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
        if (lbl_agent_badge) lv_obj_add_flag(lbl_agent_badge, LV_OBJ_FLAG_HIDDEN);
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

    bool show_badge = show_dot && g_act_agents > 1;
    if (lbl_agent_badge) {
        if (show_badge) {
            // font_styrene_12 only bakes ASCII 32-126 (see font_styrene_12.c
            // cmap) — no "×" glyph, so a plain "x" it is.
            lv_label_set_text_fmt(lbl_agent_badge, "x%d", g_act_agents);
            lv_obj_clear_flag(lbl_agent_badge, LV_OBJ_FLAG_HIDDEN);
            lv_obj_update_layout(act_dot);
            lv_obj_align_to(lbl_agent_badge, act_dot, LV_ALIGN_OUT_LEFT_MID, -4, 0);
        } else {
            lv_obj_add_flag(lbl_agent_badge, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void ui_tick_anim(void) {
    uint32_t now = lv_tick_get();

    if (dots_hide_ms && now > dots_hide_ms) {
        dots_hide_ms = 0;
        for (int i = 0; i < MAX_DOTS; i++) lv_obj_add_flag(dot_objs[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Freshness pill + clock — refresh roughly once per second, on every screen.
    static uint32_t status_last_ms = 0;
    if (now - status_last_ms >= 1000) {
        status_last_ms = now;
        refresh_status_label();
        timer_tick();  // runs on any screen so the break banner still fires
        if (current_screen == SCREEN_CLOCK) refresh_clock(false);
    }

    // Trend graphs advance on a 15-minute cadence, so a slow refresh is
    // plenty — it keeps the right-hand edge tracking the live reading
    // without rebuilding polylines on every sensor poll.
    if (current_screen == SCREEN_SENSOR || current_screen == SCREEN_SENSOR_GRAPH) {
        static uint32_t trend_last_ms = 0;
        if (now - trend_last_ms >= 15000) {
            trend_last_ms = now;
            refresh_trends_if_visible();
        }
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
    lv_obj_add_flag(trend_container, LV_OBJ_FLAG_HIDDEN);
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
    case SCREEN_SENSOR:
        lv_obj_clear_flag(sensor_container, LV_OBJ_FLAG_HIDDEN);
        refresh_sensor_spark();
        break;
    case SCREEN_SENSOR_GRAPH:
        lv_obj_clear_flag(trend_container, LV_OBJ_FLAG_HIDDEN);
        refresh_trend_screen();
        break;
    case SCREEN_BLUETOOTH:  lv_obj_clear_flag(ble_container, LV_OBJ_FLAG_HIDDEN); break;
    default: break;
    }

    // Logo is 80×80 — too large for 135px screen; keep permanently hidden.
    if (logo_img) lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
    apply_battery_visibility();
    refresh_status_label();
    refresh_screen_dots(screen);
}

void ui_cycle_screen(void) {
    screen_t next = current_screen;
    do {
        if (next == SCREEN_CLOCK)          next = SCREEN_SENSOR;
        else if (next == SCREEN_SENSOR)    next = SCREEN_SENSOR_GRAPH;
        else if (next == SCREEN_SENSOR_GRAPH) next = SCREEN_USAGE;
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
        if (!screen_is_populated(next)) continue;
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
