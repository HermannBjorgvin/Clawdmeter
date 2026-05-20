#include "ui_freenove.h"
#include "freenove_board.h"
#include <lvgl.h>
#include "theme.h"

LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);

#define COL_BG        THEME_BG
#define COL_PANEL     THEME_PANEL
#define COL_TEXT      THEME_TEXT
#define COL_DIM       THEME_DIM
#define COL_GREEN     THEME_GREEN
#define COL_AMBER     THEME_AMBER
#define COL_RED       THEME_RED
#define COL_BAR_BG    THEME_BAR_BG

static lv_obj_t* lbl_title;
static lv_obj_t* lbl_status;
static lv_obj_t* lbl_session_pct;
static lv_obj_t* lbl_session_reset;
static lv_obj_t* bar_session;
static lv_obj_t* lbl_weekly_pct;
static lv_obj_t* lbl_weekly_reset;
static lv_obj_t* bar_weekly;

static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

static void format_reset_time(int mins, char* buf, size_t len) {
    if (mins < 0) snprintf(buf, len, "---");
    else if (mins < 60) snprintf(buf, len, "Resets in %dm", mins);
    else if (mins < 1440) snprintf(buf, len, "Resets in %dh %dm", mins / 60, mins % 60);
    else snprintf(buf, len, "Resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
}

static lv_obj_t* make_panel(lv_obj_t* parent, int y) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, 12, y);
    lv_obj_set_size(panel, LCD_WIDTH - 24, 102);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

static lv_obj_t* make_bar(lv_obj_t* parent, int y) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, 0, y);
    lv_obj_set_size(bar, LCD_WIDTH - 48, 16);
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

static void init_usage_panel(lv_obj_t* parent, int y, const char* label,
                             lv_obj_t** out_pct, lv_obj_t** out_reset, lv_obj_t** out_bar) {
    lv_obj_t* panel = make_panel(parent, y);

    lv_obj_t* lbl = lv_label_create(panel);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, &font_styrene_20, 0);
    lv_obj_set_style_text_color(lbl, COL_DIM, 0);
    lv_obj_set_pos(lbl, 0, 0);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, &font_styrene_28, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_align(*out_pct, LV_ALIGN_TOP_RIGHT, 0, -2);

    *out_bar = make_bar(panel, 34);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, &font_styrene_20, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, 62);
}

void ui_init(void) {
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lbl_title = lv_label_create(scr);
    lv_label_set_text(lbl_title, "Usage");
    lv_obj_set_style_text_font(lbl_title, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 16);

    lbl_status = lv_label_create(scr);
    lv_label_set_text(lbl_status, "Waiting for BLE data...");
    lv_obj_set_style_text_font(lbl_status, &font_styrene_20, 0);
    lv_obj_set_style_text_color(lbl_status, COL_DIM, 0);
    lv_obj_align(lbl_status, LV_ALIGN_TOP_MID, 0, 48);

    init_usage_panel(scr, 78, "Current", &lbl_session_pct, &lbl_session_reset, &bar_session);
    init_usage_panel(scr, 192, "Weekly", &lbl_weekly_pct, &lbl_weekly_reset, &bar_weekly);
}

void ui_set_waiting_state(void) {
    lv_label_set_text(lbl_status, "Waiting for BLE data...");
}

void ui_update(const UsageData* data) {
    if (!data || !data->valid) return;

    lv_label_set_text(lbl_status, data->ok ? "Connected" : data->status);

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
