#include "../ui.h"
#include "../theme.h"
#include "../splash.h"
#include "../logo.h"
#include <lvgl.h>

LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_mono_18);

#define COL_BG      THEME_BG
#define COL_PANEL   THEME_PANEL
#define COL_TEXT    THEME_TEXT
#define COL_DIM     THEME_DIM
#define COL_ACCENT  THEME_ACCENT
#define COL_GREEN   THEME_GREEN
#define COL_AMBER   THEME_AMBER
#define COL_RED     THEME_RED
#define COL_BAR_BG  THEME_BAR_BG

// ---- Layout (320x240 landscape) ----
#define SCR_W       320
#define SCR_H       240
#define MARGIN      6
#define TOP_BAR_H   22
#define ANIM_H      20
#define PANEL_GAP   4
#define PANEL_H     ((SCR_H - TOP_BAR_H - ANIM_H - PANEL_GAP) / 2)  // 97
#define PANEL_W     (SCR_W - 2 * MARGIN)                            // 308

// ---- Usage screen widgets ----
static lv_obj_t *usage_root;
static lv_obj_t *lbl_title;
static lv_obj_t *logo_img;
static lv_image_dsc_t logo_dsc;
static lv_obj_t *bar_session,  *lbl_session_pct,  *lbl_session_reset;
static lv_obj_t *bar_weekly,   *lbl_weekly_pct,   *lbl_weekly_reset;
static lv_obj_t *lbl_anim;

// ---- Bluetooth screen widgets ----
static lv_obj_t *ble_root;
static lv_obj_t *lbl_ble_status;
static lv_obj_t *lbl_ble_device;
static lv_obj_t *lbl_ble_mac;

static screen_t current_screen = SCREEN_USAGE;

// ---- Spinner animation state ----
static uint32_t anim_last_ms  = 0;
static uint32_t anim_msg_start = 0;
static uint8_t  anim_spinner_idx = 0;
static uint8_t  anim_phase = 0;
static uint8_t  anim_msg_idx = 0;
#define ANIM_MSG_MS 4000

static const char *const spinner_frames[] = {
    "\xC2\xB7", "\xE2\x9C\xBB", "\xE2\x9C\xBD",
    "\xE2\x9C\xB6", "\xE2\x9C\xB3", "\xE2\x9C\xA2",
};
#define SPINNER_COUNT  6
#define SPINNER_PHASES (2 * (SPINNER_COUNT - 1))
static const uint16_t spinner_ms[SPINNER_COUNT] = {260, 130, 130, 130, 130, 260};

static const char *const anim_messages[] = {
    "Thinking", "Brewing", "Cooking", "Crunching", "Pondering",
    "Conjuring", "Cogitating", "Computing", "Considering",
    "Deliberating", "Forging", "Generating", "Manifesting",
    "Mulling", "Noodling", "Percolating", "Processing",
    "Ruminating", "Scheming", "Spinning", "Synthesizing",
    "Tinkering", "Wandering", "Working",
};
#define ANIM_MSG_COUNT (sizeof(anim_messages) / sizeof(anim_messages[0]))

static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

static void format_reset_time(int mins, char *buf, size_t len) {
    if (mins < 0)              snprintf(buf, len, "---");
    else if (mins < 60)        snprintf(buf, len, "Resets in %dm", mins);
    else if (mins < 1440)      snprintf(buf, len, "Resets in %dh %dm", mins / 60, mins % 60);
    else                       snprintf(buf, len, "Resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
}

// Forward decls
static void cycle_click_cb(lv_event_t *e);
static void ble_reset_click_cb(lv_event_t *e);

static lv_obj_t *make_panel(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, 6, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_pad_left(p, 10, 0);
    lv_obj_set_style_pad_right(p, 10, 0);
    lv_obj_set_style_pad_top(p, 8, 0);
    lv_obj_set_style_pad_bottom(p, 8, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_EVENT_BUBBLE);
    return p;
}

static lv_obj_t *make_bar(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *b = lv_bar_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_bar_set_range(b, 0, 100);
    lv_bar_set_value(b, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(b, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(b, 3, LV_PART_INDICATOR);
    return b;
}

static lv_obj_t *make_pill(lv_obj_t *parent, const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &font_styrene_14, 0);
    lv_obj_set_style_text_color(l, COL_TEXT, 0);
    lv_obj_set_style_bg_color(l, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(l, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(l, 10, 0);
    lv_obj_set_style_pad_right(l, 10, 0);
    lv_obj_set_style_pad_top(l, 3, 0);
    lv_obj_set_style_pad_bottom(l, 3, 0);
    return l;
}

// Inside-panel layout: % top-left, pill top-right, bar mid, reset label bot.
static void make_usage_panel(lv_obj_t *parent, int y, const char *pill_text,
                             lv_obj_t **out_pct, lv_obj_t **out_bar,
                             lv_obj_t **out_reset) {
    lv_obj_t *panel = make_panel(parent, MARGIN, y, PANEL_W, PANEL_H);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, &font_styrene_28, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    lv_obj_t *pill = make_pill(panel, pill_text);
    lv_obj_align(pill, LV_ALIGN_TOP_RIGHT, 0, 2);

    *out_bar = make_bar(panel, 0, 38, PANEL_W - 20, 12);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, &font_styrene_14, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, 58);
}

static void init_usage_screen(lv_obj_t *scr) {
    usage_root = lv_obj_create(scr);
    lv_obj_set_size(usage_root, SCR_W, SCR_H);
    lv_obj_set_pos(usage_root, 0, 0);
    lv_obj_set_style_bg_opa(usage_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_root, 0, 0);
    lv_obj_set_style_pad_all(usage_root, 0, 0);
    lv_obj_clear_flag(usage_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(usage_root, cycle_click_cb, LV_EVENT_CLICKED, NULL);

    lbl_title = lv_label_create(usage_root);
    lv_label_set_text(lbl_title, "Usage");
    lv_obj_set_style_text_font(lbl_title, &font_styrene_20, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    lv_obj_set_pos(lbl_title, MARGIN, 1);

    // Clawd logo top-right. The asset is 80x80 RGB565A8; we use LVGL's image
    // scale to render it at ~36x36 (256 = 1x, so 115 ≈ 0.45x → 36px).
    // Antialias off so the pixel art stays crisp.
    logo_dsc.header.w     = LOGO_WIDTH;
    logo_dsc.header.h     = LOGO_HEIGHT;
    logo_dsc.header.cf    = LV_COLOR_FORMAT_RGB565A8;
    logo_dsc.header.stride = LOGO_WIDTH * 2;
    logo_dsc.data         = logo_data;
    logo_dsc.data_size    = LOGO_WIDTH * LOGO_HEIGHT * 3;

    logo_img = lv_image_create(usage_root);
    lv_image_set_src(logo_img, &logo_dsc);
    lv_image_set_antialias(logo_img, false);
    lv_image_set_scale(logo_img, 115);
    // After scaling, the *bounding box* still reports the original 80px size
    // unless we also pivot. Position so the scaled 36px image's right edge
    // lands at SCR_W - MARGIN; account for the offset introduced by scaling.
    lv_obj_set_pos(logo_img, SCR_W - MARGIN - 36 - 22, -22);
    lv_obj_add_flag(logo_img, LV_OBJ_FLAG_EVENT_BUBBLE);

    int y0 = TOP_BAR_H;
    make_usage_panel(usage_root, y0,                          "Session",
                     &lbl_session_pct, &bar_session, &lbl_session_reset);
    make_usage_panel(usage_root, y0 + PANEL_H + PANEL_GAP,    "Weekly",
                     &lbl_weekly_pct,  &bar_weekly,  &lbl_weekly_reset);

    lbl_anim = lv_label_create(usage_root);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, &font_mono_18, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_LEFT, MARGIN, -1);
}

static void init_bluetooth_screen(lv_obj_t *scr) {
    ble_root = lv_obj_create(scr);
    lv_obj_set_size(ble_root, SCR_W, SCR_H);
    lv_obj_set_pos(ble_root, 0, 0);
    lv_obj_set_style_bg_opa(ble_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ble_root, 0, 0);
    lv_obj_set_style_pad_all(ble_root, 0, 0);
    lv_obj_clear_flag(ble_root, LV_OBJ_FLAG_SCROLLABLE);
    // Tap on empty area goes back to Usage
    lv_obj_add_event_cb(ble_root, cycle_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *t = lv_label_create(ble_root);
    lv_label_set_text(t, "Bluetooth");
    lv_obj_set_style_text_font(t, &font_styrene_20, 0);
    lv_obj_set_style_text_color(t, COL_TEXT, 0);
    lv_obj_set_pos(t, MARGIN, 1);

    lv_obj_t *info = make_panel(ble_root, MARGIN, TOP_BAR_H, PANEL_W, 96);

    lbl_ble_status = lv_label_create(info);
    lv_label_set_text(lbl_ble_status, "Initializing...");
    lv_obj_set_style_text_font(lbl_ble_status, &font_styrene_24, 0);
    lv_obj_set_style_text_color(lbl_ble_status, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_status, 0, 0);

    lbl_ble_device = lv_label_create(info);
    lv_label_set_text(lbl_ble_device, "Device: ---");
    lv_obj_set_style_text_font(lbl_ble_device, &font_styrene_14, 0);
    lv_obj_set_style_text_color(lbl_ble_device, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_device, 0, 36);

    lbl_ble_mac = lv_label_create(info);
    lv_label_set_text(lbl_ble_mac, "Address: ---");
    lv_obj_set_style_text_font(lbl_ble_mac, &font_styrene_14, 0);
    lv_obj_set_style_text_color(lbl_ble_mac, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_mac, 0, 56);

    // Reset Bluetooth tap zone — child of ble_root, consumes the event so it
    // does NOT cycle back to Usage.
    int reset_y = TOP_BAR_H + 96 + 6;
    lv_obj_t *reset_zone = lv_obj_create(ble_root);
    lv_obj_set_pos(reset_zone, MARGIN, reset_y);
    lv_obj_set_size(reset_zone, PANEL_W, SCR_H - reset_y - 6);
    lv_obj_set_style_bg_color(reset_zone, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(reset_zone, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(reset_zone, 6, 0);
    lv_obj_set_style_border_width(reset_zone, 0, 0);
    lv_obj_clear_flag(reset_zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(reset_zone, ble_reset_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *rl = lv_label_create(reset_zone);
    lv_label_set_text(rl, "Reset Bluetooth");
    lv_obj_set_style_text_font(rl, &font_styrene_20, 0);
    lv_obj_set_style_text_color(rl, COL_TEXT, 0);
    lv_obj_center(rl);

    lv_obj_add_flag(ble_root, LV_OBJ_FLAG_HIDDEN);
}

// ---- Touch handlers ----

static void cycle_click_cb(lv_event_t *e) {
    (void)e;
    ui_cycle_screen();
}

static void ble_reset_click_cb(lv_event_t *e) {
    (void)e;
    ble_clear_bonds();
}

// ---- Public API ----

void ui_init(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    init_usage_screen(scr);
    init_bluetooth_screen(scr);

    // Splash creates its own container; we attach the cycle tap handler so a
    // tap anywhere on the splash advances to the Bluetooth screen.
    splash_init(scr);
    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), cycle_click_cb, LV_EVENT_CLICKED, NULL);
    }
}

void ui_update(const UsageData *data) {
    if (!data->valid) return;

    int sp = (int)(data->session_pct + 0.5f);
    lv_label_set_text_fmt(lbl_session_pct, "%d%%", sp);
    lv_bar_set_value(bar_session, sp, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_session, pct_color(data->session_pct), LV_PART_INDICATOR);

    char buf[48];
    format_reset_time(data->session_reset_mins, buf, sizeof(buf));
    lv_label_set_text(lbl_session_reset, buf);

    int wp = (int)(data->weekly_pct + 0.5f);
    lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", wp);
    lv_bar_set_value(bar_weekly, wp, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_weekly, pct_color(data->weekly_pct), LV_PART_INDICATOR);

    format_reset_time(data->weekly_reset_mins, buf, sizeof(buf));
    lv_label_set_text(lbl_weekly_reset, buf);
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

        static char buf[64];
        snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
                 spinner_frames[anim_spinner_idx],
                 anim_messages[anim_msg_idx]);
        lv_label_set_text(lbl_anim, buf);
    }
}

void ui_show_screen(screen_t screen) {
    lv_obj_add_flag(usage_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ble_root,   LV_OBJ_FLAG_HIDDEN);
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:    splash_show(); break;
    case SCREEN_USAGE:     lv_obj_clear_flag(usage_root, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_BLUETOOTH: lv_obj_clear_flag(ble_root,   LV_OBJ_FLAG_HIDDEN); break;
    default:               lv_obj_clear_flag(usage_root, LV_OBJ_FLAG_HIDDEN); screen = SCREEN_USAGE; break;
    }
    current_screen = screen;
}

// Tap cycles: Usage -> Splash -> Bluetooth -> Usage
void ui_cycle_screen(void) {
    screen_t next;
    switch (current_screen) {
    case SCREEN_USAGE:     next = SCREEN_SPLASH;    break;
    case SCREEN_SPLASH:    next = SCREEN_BLUETOOTH; break;
    case SCREEN_BLUETOOTH: next = SCREEN_USAGE;     break;
    default:               next = SCREEN_USAGE;     break;
    }
    ui_show_screen(next);
}

screen_t ui_get_current_screen(void) { return current_screen; }

void ui_update_ble_status(ble_state_t state, const char *name, const char *mac) {
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
