// Clawdmeter UI for 640×172 landscape (LVGL 8).
//
// Four tiles laid out horizontally, swipe-navigated:
//   0: Usage     - session/weekly bars + Day/Week/Month cost
//   1: Sessions  - per-CC list (project, state badge, waiting message)
//   2: Tokens    - per-bucket (Input/Output/Cache R/W) token+cost matrix
//                  for Day/Week/Month
//   3: Settings  - BLE state, MAC, heap, version
//
// On top of all of these, a full-screen "CLAUDE is waiting" overlay appears
// whenever the daemon ships a non-empty attn_msg, and disappears as soon as
// the message clears.
//
// Design tokens come from clawdmeter_theme.h (mirrors firmware/src/theme.h on
// the 1.8" build): true black BG, dark #1f1f1e cards, cream text, terra-cotta
// accent. Bar tracks use #2a2a28, fill is green / amber / red by threshold.

#include "clawdmeter_ui.h"
#include "clawdmeter_theme.h"
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "logo.h"
#include <stdio.h>
#include <string.h>

#define CANVAS_W 640
#define CANVAS_H 172
#define MARGIN   8

// ---- Tile geometry (shared) ----
#define TOP_ROW_H    32
#define CARD_Y       34
#define CARD_H       102
#define CARD_GAP     8
#define FOOTER_Y     148

// ---- Usage tile cards ----
#define U_CARD_W_BARS  396
#define U_CARD_W_COST  (CANVAS_W - MARGIN*2 - U_CARD_W_BARS - CARD_GAP)
#define U_CARD_X_BARS  MARGIN
#define U_CARD_X_COST  (U_CARD_X_BARS + U_CARD_W_BARS + CARD_GAP)
#define U_BAR_W (U_CARD_W_BARS - 24)

// ---- Tileview + tiles ----
static lv_obj_t *tileview;
static lv_obj_t *tile_usage, *tile_sessions, *tile_tokens, *tile_settings;

// ---- Usage tile widgets ----
static lv_obj_t *bar_sess, *bar_week;
static lv_obj_t *lbl_sess_pct, *lbl_week_pct;
static lv_obj_t *lbl_sess_sub, *lbl_week_sub;
static lv_obj_t *val_cost_day, *val_cost_week, *val_cost_month;
static lv_obj_t *lbl_cost_proj, *lbl_cost_burn;
static lv_obj_t *u_lbl_batt, *u_batt_fill, *u_lbl_sessions_chip, *u_batt_bolt;
static lv_obj_t *lbl_footer;

// ---- Sessions tile widgets ----
#define MAX_SESS_ROWS 6
static lv_obj_t *s_lbl_batt, *s_batt_fill, *s_lbl_sessions_chip, *s_batt_bolt;
static lv_obj_t *s_row[MAX_SESS_ROWS];        // container per row
static lv_obj_t *s_row_dot[MAX_SESS_ROWS];    // colored circle (state)
static lv_obj_t *s_row_name[MAX_SESS_ROWS];
static lv_obj_t *s_row_msg[MAX_SESS_ROWS];
static lv_obj_t *s_empty_label;               // shown when count == 0

// ---- Tokens tile widgets ----
// 4 buckets × 3 windows = 12 cells, plus header row + totals row.
static const char *tok_bucket_names[4] = {"Input", "Output", "Cache R", "Cache W"};
static lv_obj_t *t_lbl_batt, *t_batt_fill, *t_lbl_sessions_chip, *t_batt_bolt;
static lv_obj_t *t_cell[4][3];      // cost cells [bucket][window]
static lv_obj_t *t_total[3];        // bottom-row total per window
static lv_obj_t *t_burn_proj;       // burn rate + projection line

// ---- Settings tile widgets ----
static lv_obj_t *set_ble_state, *set_ble_mac, *set_heap, *set_version;

// ---- Attention overlay ----
static lv_obj_t *attn_overlay;
static lv_obj_t *attn_msg_label;
static char      last_attn[96] = {0};

// ---- State ----
static lv_timer_t *mock_timer = NULL;
static int real_data_received = 0;

// ============================================================================
// Helpers
// ============================================================================

static lv_color_t bar_color_for(int pct) {
    if (pct >= 90) return COL_RED;
    if (pct >= 70) return COL_AMBER;
    return COL_GREEN;
}

static void style_bar(lv_obj_t *bar) {
    lv_obj_set_style_bg_color(bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
}

static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 10, 0);
    lv_obj_set_style_pad_all(c, 10, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static void fmt_money(char *buf, size_t n, long cents) {
    long whole = cents / 100;
    long frac  = cents % 100; if (frac < 0) frac = -frac;
    if (whole >= 10000) {
        snprintf(buf, n, "$%ld,%03ld.%02ld", whole / 1000, whole % 1000, frac);
    } else {
        snprintf(buf, n, "$%ld.%02ld", whole, frac);
    }
}

// Shorten "1234567" → "1.2M", "12345" → "12.3K"
static void fmt_tokens(char *buf, size_t n, long long t) {
    if (t < 0) { snprintf(buf, n, "-"); return; }
    if (t < 1000)             snprintf(buf, n, "%lld", t);
    else if (t < 1000000)     snprintf(buf, n, "%.1fK", t / 1000.0);
    else if (t < 1000000000LL)snprintf(buf, n, "%.1fM", t / 1000000.0);
    else                      snprintf(buf, n, "%.1fB", t / 1000000000.0);
}

// "1h 29m" / "47m" / "2d 4h" - pick a reasonable granularity.
static void fmt_remaining(char *buf, size_t n, int mins) {
    if (mins < 0) { snprintf(buf, n, "-"); return; }
    if (mins < 60)        snprintf(buf, n, "%dm left", mins);
    else if (mins < 1440) snprintf(buf, n, "%dh %dm left", mins / 60, mins % 60);
    else                  snprintf(buf, n, "%dd %dh left", mins / 1440, (mins % 1440) / 60);
}

// Build a battery indicator (body + tip + fill + bolt) inside a tile's top
// row. Output handles go to the *_out pointers so we can update across tiles.
static void make_top_status(lv_obj_t *tile, const char *title,
                            lv_obj_t **sess_chip_out,
                            lv_obj_t **batt_fill_out,
                            lv_obj_t **batt_bolt_out,
                            lv_obj_t **batt_lbl_out)
{
    lv_obj_t *logo = lv_obj_create(tile);
    lv_obj_remove_style_all(logo);
    lv_obj_set_size(logo, 22, 22);
    lv_obj_set_pos(logo, MARGIN, 4);
    lv_obj_set_style_bg_color(logo, COL_ACCENT, 0);
    lv_obj_set_style_bg_opa(logo, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(logo, 4, 0);

    lv_obj_t *t = lv_label_create(tile);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, COL_TEXT, 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(t, 40, 4);

    *sess_chip_out = lv_label_create(tile);
    lv_label_set_text(*sess_chip_out, "0 sess");
    lv_obj_set_style_text_color(*sess_chip_out, COL_DIM, 0);
    lv_obj_set_style_text_font(*sess_chip_out, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(*sess_chip_out, 148, 9);

    lv_obj_t *batt_body = lv_obj_create(tile);
    lv_obj_remove_style_all(batt_body);
    lv_obj_set_size(batt_body, 20, 12);
    lv_obj_set_pos(batt_body, 606, 8);
    lv_obj_set_style_bg_opa(batt_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(batt_body, COL_TEXT, 0);
    lv_obj_set_style_border_width(batt_body, 1, 0);
    lv_obj_set_style_radius(batt_body, 2, 0);

    lv_obj_t *batt_tip = lv_obj_create(tile);
    lv_obj_remove_style_all(batt_tip);
    lv_obj_set_size(batt_tip, 2, 6);
    lv_obj_set_pos(batt_tip, 626, 11);
    lv_obj_set_style_bg_color(batt_tip, COL_TEXT, 0);
    lv_obj_set_style_bg_opa(batt_tip, LV_OPA_COVER, 0);

    *batt_fill_out = lv_obj_create(tile);
    lv_obj_remove_style_all(*batt_fill_out);
    lv_obj_set_size(*batt_fill_out, 16, 8);
    lv_obj_set_pos(*batt_fill_out, 608, 10);
    lv_obj_set_style_bg_color(*batt_fill_out, COL_GREEN, 0);
    lv_obj_set_style_bg_opa(*batt_fill_out, LV_OPA_COVER, 0);

    *batt_bolt_out = lv_label_create(tile);
    lv_label_set_text(*batt_bolt_out, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_color(*batt_bolt_out, COL_BG, 0);
    lv_obj_set_style_text_font(*batt_bolt_out, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(*batt_bolt_out, 613, 8);
    lv_obj_add_flag(*batt_bolt_out, LV_OBJ_FLAG_HIDDEN);

    *batt_lbl_out = lv_label_create(tile);
    lv_label_set_text(*batt_lbl_out, "0%");
    lv_obj_set_style_text_color(*batt_lbl_out, COL_TEXT, 0);
    lv_obj_set_style_text_font(*batt_lbl_out, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(*batt_lbl_out, 560, 9);
}

// Push battery/session-count state to a tile's top-row widgets.
static void paint_top(lv_obj_t *chip, lv_obj_t *fill, lv_obj_t *bolt,
                      lv_obj_t *batt_lbl,
                      int batt_pct, int charging, int sessions_count)
{
    if (!chip || !fill || !bolt || !batt_lbl) return;
    lv_label_set_text_fmt(chip, "%d sess", sessions_count);
    lv_label_set_text_fmt(batt_lbl, "%d%%", batt_pct);
    lv_obj_set_width(fill, (16 * batt_pct) / 100);
    lv_color_t bc = charging ? COL_ACCENT
                  : (batt_pct >= 50) ? COL_GREEN
                  : (batt_pct >= 20) ? COL_AMBER
                  : COL_RED;
    lv_obj_set_style_bg_color(fill, bc, LV_PART_MAIN);
    if (charging) lv_obj_clear_flag(bolt, LV_OBJ_FLAG_HIDDEN);
    else          lv_obj_add_flag (bolt, LV_OBJ_FLAG_HIDDEN);
}

// ============================================================================
// Usage tile
// ============================================================================

static void build_usage_tile(lv_obj_t *tile) {
    make_top_status(tile, "Usage", &u_lbl_sessions_chip,
                    &u_batt_fill, &u_batt_bolt, &u_lbl_batt);

    // ---- Left card: stacked session + weekly ----
    lv_obj_t *card_b = make_card(tile, U_CARD_X_BARS, CARD_Y, U_CARD_W_BARS, CARD_H);

    lv_obj_t *l_s = lv_label_create(card_b);
    lv_label_set_text(l_s, "Session");
    lv_obj_set_style_text_color(l_s, COL_DIM, 0);
    lv_obj_set_style_text_font(l_s, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(l_s, 0, 0);

    lbl_sess_pct = lv_label_create(card_b);
    lv_label_set_text(lbl_sess_pct, "0%");
    lv_obj_set_style_text_color(lbl_sess_pct, COL_TEXT, 0);
    lv_obj_set_style_text_font(lbl_sess_pct, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(lbl_sess_pct, 80, -3);

    lbl_sess_sub = lv_label_create(card_b);
    lv_label_set_text(lbl_sess_sub, "-");
    lv_obj_set_style_text_color(lbl_sess_sub, COL_DIM, 0);
    lv_obj_set_style_text_font(lbl_sess_sub, &lv_font_montserrat_12, 0);
    lv_obj_set_width(lbl_sess_sub, 200);
    lv_obj_set_style_text_align(lbl_sess_sub, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(lbl_sess_sub, U_BAR_W - 200, 4);

    bar_sess = lv_bar_create(card_b);
    lv_obj_set_size(bar_sess, U_BAR_W, 8);
    lv_obj_set_pos(bar_sess, 0, 26);
    lv_bar_set_range(bar_sess, 0, 100);
    style_bar(bar_sess);

    lv_obj_t *l_w = lv_label_create(card_b);
    lv_label_set_text(l_w, "Weekly");
    lv_obj_set_style_text_color(l_w, COL_DIM, 0);
    lv_obj_set_style_text_font(l_w, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(l_w, 0, 44);

    lbl_week_pct = lv_label_create(card_b);
    lv_label_set_text(lbl_week_pct, "0%");
    lv_obj_set_style_text_color(lbl_week_pct, COL_TEXT, 0);
    lv_obj_set_style_text_font(lbl_week_pct, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(lbl_week_pct, 80, 41);

    lbl_week_sub = lv_label_create(card_b);
    lv_label_set_text(lbl_week_sub, "-");
    lv_obj_set_style_text_color(lbl_week_sub, COL_DIM, 0);
    lv_obj_set_style_text_font(lbl_week_sub, &lv_font_montserrat_12, 0);
    lv_obj_set_width(lbl_week_sub, 200);
    lv_obj_set_style_text_align(lbl_week_sub, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(lbl_week_sub, U_BAR_W - 200, 48);

    bar_week = lv_bar_create(card_b);
    lv_obj_set_size(bar_week, U_BAR_W, 8);
    lv_obj_set_pos(bar_week, 0, 70);
    lv_bar_set_range(bar_week, 0, 100);
    style_bar(bar_week);

    // ---- Right card: cost rows + projection + burn rate ----
    lv_obj_t *card_c = make_card(tile, U_CARD_X_COST, CARD_Y, U_CARD_W_COST, CARD_H);
    struct Row { const char *l; lv_obj_t **out; int y; };
    struct Row rows[3] = {
        {"Today", &val_cost_day,   0  },
        {"Week",  &val_cost_week,  18 },
        {"Month", &val_cost_month, 36 },
    };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *lab = lv_label_create(card_c);
        lv_label_set_text(lab, rows[i].l);
        lv_obj_set_style_text_color(lab, COL_DIM, 0);
        lv_obj_set_style_text_font(lab, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(lab, 0, rows[i].y);

        *rows[i].out = lv_label_create(card_c);
        lv_label_set_text(*rows[i].out, "$-");
        lv_obj_set_style_text_color(*rows[i].out, COL_TEXT, 0);
        lv_obj_set_style_text_font(*rows[i].out, &lv_font_montserrat_14, 0);
        lv_obj_set_width(*rows[i].out, U_CARD_W_COST - 30);
        lv_obj_set_style_text_align(*rows[i].out, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(*rows[i].out, 0, rows[i].y);
    }

    lbl_cost_proj = lv_label_create(card_c);
    lv_label_set_text(lbl_cost_proj, "proj -");
    lv_obj_set_style_text_color(lbl_cost_proj, COL_ACCENT, 0);
    lv_obj_set_style_text_font(lbl_cost_proj, &lv_font_montserrat_12, 0);
    lv_obj_set_width(lbl_cost_proj, U_CARD_W_COST - 30);
    lv_obj_set_style_text_align(lbl_cost_proj, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(lbl_cost_proj, 0, 60);

    lbl_cost_burn = lv_label_create(card_c);
    lv_label_set_text(lbl_cost_burn, "burn -");
    lv_obj_set_style_text_color(lbl_cost_burn, COL_DIM, 0);
    lv_obj_set_style_text_font(lbl_cost_burn, &lv_font_montserrat_12, 0);
    lv_obj_set_width(lbl_cost_burn, U_CARD_W_COST - 30);
    lv_obj_set_style_text_align(lbl_cost_burn, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(lbl_cost_burn, 0, 74);

    lbl_footer = lv_label_create(tile);
    lv_label_set_text(lbl_footer, "BLE: advertising - waiting for daemon");
    lv_obj_set_style_text_color(lbl_footer, COL_DIM, 0);
    lv_obj_set_style_text_font(lbl_footer, &lv_font_montserrat_12, 0);
    lv_obj_align(lbl_footer, LV_ALIGN_TOP_MID, 0, FOOTER_Y);
}

// ============================================================================
// Sessions tile
// ============================================================================

static lv_color_t sess_state_color(int s) {
    switch (s) {
        case SESS_WORKING_S: return COL_ACCENT;
        case SESS_WAITING_S: return COL_RED;
        default:             return COL_DIM;   // idle
    }
}
static const char *sess_state_label(int s) {
    switch (s) {
        case SESS_WORKING_S: return "working";
        case SESS_WAITING_S: return "waiting";
        default:             return "idle";
    }
}

static void build_sessions_tile(lv_obj_t *tile) {
    make_top_status(tile, "Sessions", &s_lbl_sessions_chip,
                    &s_batt_fill, &s_batt_bolt, &s_lbl_batt);

    lv_obj_t *card = make_card(tile, MARGIN, CARD_Y, CANVAS_W - MARGIN*2, CARD_H);

    // 3 rows × 2 columns of session entries (6 max). Each cell ~206 wide × 26 tall.
    const int row_h = 26;
    const int col_w = (CANVAS_W - MARGIN*2 - 20) / 2; // 290-ish
    for (int i = 0; i < MAX_SESS_ROWS; i++) {
        int col = i % 2;
        int row = i / 2;
        int x = col * (col_w + 10);
        int y = row * (row_h + 2);

        s_row[i] = lv_obj_create(card);
        lv_obj_remove_style_all(s_row[i]);
        lv_obj_set_pos(s_row[i], x, y);
        lv_obj_set_size(s_row[i], col_w, row_h);
        lv_obj_clear_flag(s_row[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);

        s_row_dot[i] = lv_obj_create(s_row[i]);
        lv_obj_remove_style_all(s_row_dot[i]);
        lv_obj_set_size(s_row_dot[i], 8, 8);
        lv_obj_set_pos(s_row_dot[i], 0, 6);
        lv_obj_set_style_bg_color(s_row_dot[i], COL_DIM, 0);
        lv_obj_set_style_bg_opa(s_row_dot[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_row_dot[i], 4, 0);

        s_row_name[i] = lv_label_create(s_row[i]);
        lv_label_set_text(s_row_name[i], "-");
        lv_obj_set_style_text_color(s_row_name[i], COL_TEXT, 0);
        lv_obj_set_style_text_font(s_row_name[i], &lv_font_montserrat_14, 0);
        lv_obj_set_pos(s_row_name[i], 16, 0);

        s_row_msg[i] = lv_label_create(s_row[i]);
        lv_label_set_text(s_row_msg[i], "");
        lv_obj_set_style_text_color(s_row_msg[i], COL_DIM, 0);
        lv_obj_set_style_text_font(s_row_msg[i], &lv_font_montserrat_12, 0);
        lv_obj_set_pos(s_row_msg[i], 16, 14);
        lv_label_set_long_mode(s_row_msg[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_row_msg[i], col_w - 18);
    }

    s_empty_label = lv_label_create(card);
    lv_label_set_text(s_empty_label, "No active Claude sessions");
    lv_obj_set_style_text_color(s_empty_label, COL_DIM, 0);
    lv_obj_set_style_text_font(s_empty_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_empty_label, LV_ALIGN_CENTER, 0, 0);
}

// ============================================================================
// Tokens tile
// ============================================================================

static void build_tokens_tile(lv_obj_t *tile) {
    make_top_status(tile, "Cost", &t_lbl_sessions_chip,
                    &t_batt_fill, &t_batt_bolt, &t_lbl_batt);

    lv_obj_t *card = make_card(tile, MARGIN, CARD_Y, CANVAS_W - MARGIN*2, CARD_H);

    // Column layout: label | Day | Week | Month
    const int inner_w = CANVAS_W - MARGIN*2 - 20;   // card inner width
    const int col_label_w = 90;
    const int col_w = (inner_w - col_label_w) / 3;
    const int row_h = 14;
    const int header_y = 0;
    const int body_y   = 16;

    // Header row
    const char *header[3] = {"Day", "Week", "Month"};
    for (int c = 0; c < 3; c++) {
        lv_obj_t *h = lv_label_create(card);
        lv_label_set_text(h, header[c]);
        lv_obj_set_style_text_color(h, COL_DIM, 0);
        lv_obj_set_style_text_font(h, &lv_font_montserrat_12, 0);
        lv_obj_set_pos(h, col_label_w + c * col_w, header_y);
        lv_obj_set_width(h, col_w - 4);
        lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_RIGHT, 0);
    }

    // Bucket rows (Input / Output / Cache R / Cache W)
    for (int b = 0; b < 4; b++) {
        int y = body_y + b * row_h;
        lv_obj_t *lab = lv_label_create(card);
        lv_label_set_text(lab, tok_bucket_names[b]);
        lv_obj_set_style_text_color(lab, COL_DIM, 0);
        lv_obj_set_style_text_font(lab, &lv_font_montserrat_12, 0);
        lv_obj_set_pos(lab, 0, y);

        for (int c = 0; c < 3; c++) {
            t_cell[b][c] = lv_label_create(card);
            lv_label_set_text(t_cell[b][c], "-");
            lv_obj_set_style_text_color(t_cell[b][c], COL_TEXT, 0);
            lv_obj_set_style_text_font(t_cell[b][c], &lv_font_montserrat_12, 0);
            lv_obj_set_pos(t_cell[b][c], col_label_w + c * col_w, y);
            lv_obj_set_width(t_cell[b][c], col_w - 4);
            lv_obj_set_style_text_align(t_cell[b][c], LV_TEXT_ALIGN_RIGHT, 0);
        }
    }

    // Total row
    int total_y = body_y + 4 * row_h + 2;
    lv_obj_t *tot_lbl = lv_label_create(card);
    lv_label_set_text(tot_lbl, "Total");
    lv_obj_set_style_text_color(tot_lbl, COL_TEXT, 0);
    lv_obj_set_style_text_font(tot_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(tot_lbl, 0, total_y);
    for (int c = 0; c < 3; c++) {
        t_total[c] = lv_label_create(card);
        lv_label_set_text(t_total[c], "$-");
        lv_obj_set_style_text_color(t_total[c], COL_TEXT, 0);
        lv_obj_set_style_text_font(t_total[c], &lv_font_montserrat_14, 0);
        lv_obj_set_pos(t_total[c], col_label_w + c * col_w, total_y);
        lv_obj_set_width(t_total[c], col_w - 4);
        lv_obj_set_style_text_align(t_total[c], LV_TEXT_ALIGN_RIGHT, 0);
    }

    // Burn + projection (Day)
    t_burn_proj = lv_label_create(card);
    lv_label_set_text(t_burn_proj, "burn - | proj -");
    lv_obj_set_style_text_color(t_burn_proj, COL_ACCENT, 0);
    lv_obj_set_style_text_font(t_burn_proj, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(t_burn_proj, 0, total_y + 18);
}

// ============================================================================
// Settings tile
// ============================================================================

static void build_settings_tile(lv_obj_t *tile) {
    // Settings has its own top row but uses the Usage handles - these aren't
    // re-used for battery (we re-paint all three tiles' top rows in
    // apply_values).
    static lv_obj_t *set_chip, *set_fill, *set_bolt, *set_lbl_batt;
    make_top_status(tile, "Settings", &set_chip, &set_fill, &set_bolt, &set_lbl_batt);
    (void)set_chip; (void)set_fill; (void)set_bolt; (void)set_lbl_batt;

    lv_obj_t *card = make_card(tile, MARGIN, CARD_Y, CANVAS_W - MARGIN*2, CARD_H);
    struct Row { const char *k; lv_obj_t **out; int y; };
    set_ble_state = NULL; set_ble_mac = NULL; set_heap = NULL; set_version = NULL;
    struct Row rows[4] = {
        {"BLE",     &set_ble_state, 0  },
        {"MAC",     &set_ble_mac,   24 },
        {"Heap",    &set_heap,      48 },
        {"Version", &set_version,   72 },
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *k = lv_label_create(card);
        lv_label_set_text(k, rows[i].k);
        lv_obj_set_style_text_color(k, COL_DIM, 0);
        lv_obj_set_style_text_font(k, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(k, 0, rows[i].y);

        *rows[i].out = lv_label_create(card);
        lv_label_set_text(*rows[i].out, "-");
        lv_obj_set_style_text_color(*rows[i].out, COL_TEXT, 0);
        lv_obj_set_style_text_font(*rows[i].out, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(*rows[i].out, 80, rows[i].y);
    }

    lv_obj_t *hint = lv_label_create(tile);
    lv_label_set_text(hint, "swipe ← back to Usage");
    lv_obj_set_style_text_color(hint, COL_DIM, 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, FOOTER_Y);
}

// ============================================================================
// Attention overlay - full-screen takeover for non-empty attn_msg
// ============================================================================

static void attn_click_cb(lv_event_t *e) {
    (void)e;
    // Tap dismisses until the daemon sends a new message.
    lv_obj_add_flag(attn_overlay, LV_OBJ_FLAG_HIDDEN);
}

// 80x80 Clawd logo, vendored from the 1.8" build (firmware/src/logo.h).
//
// The asset on disk is *planar* RGB565A8: 6400 RGB565 pixels (little-endian)
// followed by 6400 alpha bytes. The 1.8" build runs LVGL 9 which has a
// native RGB565A8 color format and uses the plane as-is. LVGL 8 (this build)
// has no planar format — its TRUE_COLOR_ALPHA expects bytes interleaved
// `[lo, hi, alpha]` per pixel. Without that interleave the alpha plane is
// ignored and we get a 80x80 colored rectangle instead of the masked Clawd
// silhouette.
//
// Convert once at boot into a DMA-capable internal-RAM buffer (80*80*3 =
// 19,200 bytes — comfortable in DRAM) and point the descriptor at it.
static lv_img_dsc_t logo_img_dsc;
static uint8_t *logo_interleaved = NULL;

static void prepare_logo(void) {
    if (logo_interleaved) return;
    logo_interleaved = (uint8_t*)heap_caps_malloc(LOGO_WIDTH * LOGO_HEIGHT * 3,
                                                  MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!logo_interleaved) return;
    const uint8_t *rgb_plane = logo_data;
    const uint8_t *alpha_plane = logo_data + LOGO_WIDTH * LOGO_HEIGHT * 2;
    uint8_t *dst = logo_interleaved;
    // Asset is encoded as little-endian RGB565 (low byte first per logo.h
    // comment). This LVGL 8 build uses LV_COLOR_16_SWAP=1, so the rest of
    // the rendered framebuffer is big-endian on the wire. Flip the asset's
    // byte order pixel-by-pixel so its bytes match the panel's expectation;
    // without this the high/low halves land in the wrong color channels and
    // terra-cotta renders as purple.
    for (int i = 0; i < LOGO_WIDTH * LOGO_HEIGHT; i++) {
        *dst++ = rgb_plane[i * 2 + 1];  // high byte first
        *dst++ = rgb_plane[i * 2];      // low byte second
        *dst++ = alpha_plane[i];
    }
    logo_img_dsc.header.cf       = LV_IMG_CF_TRUE_COLOR_ALPHA;
    logo_img_dsc.header.always_zero = 0;
    logo_img_dsc.header.reserved = 0;
    logo_img_dsc.header.w        = LOGO_WIDTH;
    logo_img_dsc.header.h        = LOGO_HEIGHT;
    logo_img_dsc.data_size       = LOGO_WIDTH * LOGO_HEIGHT * 3;
    logo_img_dsc.data            = logo_interleaved;
}

static void build_attention_overlay(lv_obj_t *parent) {
    prepare_logo();
    attn_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(attn_overlay);
    lv_obj_set_size(attn_overlay, CANVAS_W, CANVAS_H);
    lv_obj_set_pos(attn_overlay, 0, 0);
    lv_obj_set_style_bg_color(attn_overlay, COL_BG, 0);
    lv_obj_set_style_bg_opa(attn_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(attn_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(attn_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(attn_overlay, attn_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(attn_overlay, LV_OBJ_FLAG_HIDDEN);

    // Layout: 80x80 Clawd logo on the left, text column to its right.
    // Whole composition is centered vertically in the 172px-tall strip and
    // horizontally so it doesn't crowd the edges.
    //
    //   [   logo   ]   CLAUDE
    //   [  80x80   ]   is waiting
    //                  <message text wraps here>
    //                       tap to dismiss
    const int logo_w = 80, logo_h = 80;
    const int logo_x = 24;
    const int logo_y = (CANVAS_H - logo_h) / 2;
    const int text_x = logo_x + logo_w + 18;
    const int text_w = CANVAS_W - text_x - MARGIN;

    lv_obj_t *logo = lv_img_create(attn_overlay);
    lv_img_set_src(logo, &logo_img_dsc);
    lv_obj_set_pos(logo, logo_x, logo_y);

    lv_obj_t *title = lv_label_create(attn_overlay);
    lv_label_set_text(title, "CLAUDE");
    lv_obj_set_style_text_color(title, COL_ACCENT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(title, text_x, logo_y + 4);

    lv_obj_t *sub = lv_label_create(attn_overlay);
    lv_label_set_text(sub, "is waiting");
    lv_obj_set_style_text_color(sub, COL_DIM, 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(sub, text_x, logo_y + 30);

    attn_msg_label = lv_label_create(attn_overlay);
    lv_label_set_text(attn_msg_label, "");
    lv_obj_set_style_text_color(attn_msg_label, COL_TEXT, 0);
    lv_obj_set_style_text_font(attn_msg_label, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(attn_msg_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(attn_msg_label, text_w);
    lv_obj_set_pos(attn_msg_label, text_x, logo_y + 52);

    lv_obj_t *dismiss = lv_label_create(attn_overlay);
    lv_label_set_text(dismiss, "tap to dismiss");
    lv_obj_set_style_text_color(dismiss, COL_DIM, 0);
    lv_obj_set_style_text_font(dismiss, &lv_font_montserrat_12, 0);
    lv_obj_align(dismiss, LV_ALIGN_BOTTOM_MID, 0, -4);
}

// The default Montserrat font only contains ASCII (0x20..0x7E), so any
// Unicode the daemon ships (em-dash, middle-dot, smart quotes) renders as
// hollow boxes. Map a small set of common runes to ASCII equivalents and
// drop everything else > 0x7E rather than enabling extra font subsets.
static void utf8_to_ascii(char *out, size_t out_n, const char *in) {
    size_t o = 0;
    if (out_n == 0) return;
    while (*in && o + 1 < out_n) {
        unsigned char c = (unsigned char)*in;
        if (c < 0x80) { out[o++] = (char)c; in++; continue; }

        // Multi-byte UTF-8: peek codepoint
        unsigned cp = 0;
        int bytes = 0;
        if      ((c & 0xE0) == 0xC0) { cp = c & 0x1F; bytes = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; bytes = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; bytes = 4; }
        else { in++; continue; }   // invalid leading byte

        for (int i = 1; i < bytes && in[i]; i++) cp = (cp << 6) | (in[i] & 0x3F);
        in += bytes;

        const char *rep = NULL;
        switch (cp) {
            case 0x2013: case 0x2014: rep = "-";  break;  // en/em dash
            case 0x2018: case 0x2019: rep = "'";  break;  // smart quotes
            case 0x201C: case 0x201D: rep = "\""; break;
            case 0x2026: rep = "..."; break;
            case 0x00B7: case 0x2022: rep = "|";  break;  // middle dot, bullet
            case 0x00A0: rep = " ";   break;              // nbsp
            default: rep = "?"; break;
        }
        while (*rep && o + 1 < out_n) out[o++] = *rep++;
    }
    out[o] = '\0';
}

void clawdmeter_set_attention(const char *msg) {
    if (!attn_overlay) return;
    if (!msg || !msg[0]) {
        lv_obj_add_flag(attn_overlay, LV_OBJ_FLAG_HIDDEN);
        last_attn[0] = 0;
        return;
    }
    char clean[96];
    utf8_to_ascii(clean, sizeof(clean), msg);
    if (strcmp(last_attn, clean) == 0 && !lv_obj_has_flag(attn_overlay, LV_OBJ_FLAG_HIDDEN)) {
        return; // unchanged + already visible
    }
    snprintf(last_attn, sizeof(last_attn), "%s", clean);
    lv_label_set_text(attn_msg_label, clean);
    lv_obj_clear_flag(attn_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(attn_overlay);
}

// ============================================================================
// Update API
// ============================================================================

static void paint_all_top_rows(int batt_pct, int charging, int sessions_count) {
    paint_top(u_lbl_sessions_chip, u_batt_fill, u_batt_bolt, u_lbl_batt,
              batt_pct, charging, sessions_count);
    paint_top(s_lbl_sessions_chip, s_batt_fill, s_batt_bolt, s_lbl_batt,
              batt_pct, charging, sessions_count);
    paint_top(t_lbl_sessions_chip, t_batt_fill, t_batt_bolt, t_lbl_batt,
              batt_pct, charging, sessions_count);
}

static void update_state(int sess_pct, int week_pct,
                         int sess_reset_mins, int week_reset_mins,
                         int batt_pct, int charging,
                         long day_c, long week_c, long month_c,
                         long proj_c, long burn_c,
                         int sessions_count)
{
    lv_bar_set_value(bar_sess, sess_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_sess, bar_color_for(sess_pct), LV_PART_INDICATOR);
    lv_label_set_text_fmt(lbl_sess_pct, "%d%%", sess_pct);

    lv_bar_set_value(bar_week, week_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_week, bar_color_for(week_pct), LV_PART_INDICATOR);
    lv_label_set_text_fmt(lbl_week_pct, "%d%%", week_pct);

    char buf[32];
    fmt_remaining(buf, sizeof(buf), sess_reset_mins);
    lv_label_set_text(lbl_sess_sub, buf);
    fmt_remaining(buf, sizeof(buf), week_reset_mins);
    lv_label_set_text(lbl_week_sub, buf);

    if (day_c   >= 0) { fmt_money(buf, sizeof(buf), day_c);   lv_label_set_text(val_cost_day,   buf); }
    if (week_c  >= 0) { fmt_money(buf, sizeof(buf), week_c);  lv_label_set_text(val_cost_week,  buf); }
    if (month_c >= 0) { fmt_money(buf, sizeof(buf), month_c); lv_label_set_text(val_cost_month, buf); }
    if (proj_c  >= 0) { fmt_money(buf, sizeof(buf), proj_c);
                        lv_label_set_text_fmt(lbl_cost_proj, "proj %s day", buf); }
    if (burn_c  >= 0) { fmt_money(buf, sizeof(buf), burn_c);
                        lv_label_set_text_fmt(lbl_cost_burn, "burn %s/hr", buf); }

    paint_all_top_rows(batt_pct, charging, sessions_count);
}

void clawdmeter_apply_values(int sess_pct, int week_pct,
                             int sess_reset_mins, int week_reset_mins,
                             int batt_pct, int charging,
                             long day_cents, long week_cents, long month_cents,
                             long proj_day_cents,
                             long burn_rate_cents_per_hr,
                             int sessions_count)
{
    real_data_received = 1;
    update_state(sess_pct, week_pct, sess_reset_mins, week_reset_mins,
                 batt_pct, charging,
                 day_cents, week_cents, month_cents,
                 proj_day_cents, burn_rate_cents_per_hr,
                 sessions_count);
}

void clawdmeter_apply_tokens(long long tok_d[4], long cents_d[4],
                             long long tok_w[4], long cents_w[4],
                             long long tok_m[4], long cents_m[4])
{
    char buf[24];
    long total_d = 0, total_w = 0, total_m = 0;
    for (int b = 0; b < 4; b++) {
        // Day
        char tb[16]; fmt_tokens(tb, sizeof(tb), tok_d[b]);
        char cb[16]; fmt_money(cb, sizeof(cb), cents_d[b]);
        snprintf(buf, sizeof(buf), "%s | %s", tb, cb);
        lv_label_set_text(t_cell[b][0], buf);
        total_d += cents_d[b];

        // Week
        fmt_tokens(tb, sizeof(tb), tok_w[b]);
        fmt_money(cb, sizeof(cb), cents_w[b]);
        snprintf(buf, sizeof(buf), "%s | %s", tb, cb);
        lv_label_set_text(t_cell[b][1], buf);
        total_w += cents_w[b];

        // Month
        fmt_tokens(tb, sizeof(tb), tok_m[b]);
        fmt_money(cb, sizeof(cb), cents_m[b]);
        snprintf(buf, sizeof(buf), "%s | %s", tb, cb);
        lv_label_set_text(t_cell[b][2], buf);
        total_m += cents_m[b];
    }
    fmt_money(buf, sizeof(buf), total_d); lv_label_set_text(t_total[0], buf);
    fmt_money(buf, sizeof(buf), total_w); lv_label_set_text(t_total[1], buf);
    fmt_money(buf, sizeof(buf), total_m); lv_label_set_text(t_total[2], buf);
}

void clawdmeter_apply_sessions(const clawdmeter_session_t *list, int count) {
    if (count <= 0) {
        for (int i = 0; i < MAX_SESS_ROWS; i++) lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);
    int show = count > MAX_SESS_ROWS ? MAX_SESS_ROWS : count;
    for (int i = 0; i < MAX_SESS_ROWS; i++) {
        if (i < show) {
            lv_obj_clear_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(s_row_dot[i], sess_state_color(list[i].state), 0);
            lv_label_set_text(s_row_name[i], list[i].proj);
            const char *msg = list[i].msg[0] ? list[i].msg : sess_state_label(list[i].state);
            lv_label_set_text(s_row_msg[i], msg);
        } else {
            lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void clawdmeter_set_status(const char *status) {
    if (!lbl_footer || !status) return;
    char clean[96];
    utf8_to_ascii(clean, sizeof(clean), status);
    lv_label_set_text(lbl_footer, clean);
}

void clawdmeter_settings_set_ble(const char *state, const char *mac) {
    if (set_ble_state && state) lv_label_set_text(set_ble_state, state);
    if (set_ble_mac && mac)     lv_label_set_text(set_ble_mac, mac);
}

// ============================================================================
// Mock + heap timers
// ============================================================================

static void mock_cycle_cb(lv_timer_t *t) {
    (void)t;
    if (real_data_received) {
        if (mock_timer) { lv_timer_del(mock_timer); mock_timer = NULL; }
        return;
    }
    static int idx = 0;
    struct M { int s, w, sr, wr, b, c, n; long d, wk, m, pj, br; };
    static const struct M states[] = {
        { 47, 31,  89, 8640, 82, 1, 3,   142,    412,    2840,   410,   4245 },
        { 76, 58,  41, 4320, 64, 0, 2,   320,    680,    3120,   680,   8210 },
        { 93, 71,  12, 1800, 24, 0, 5,   560,    940,    4170,   940,  15120 },
        {  4,  3, 250, 9999, 99, 1, 0,     4,     40,     310,    40,    250 },
    };
    const struct M *m = &states[idx];
    update_state(m->s, m->w, m->sr, m->wr, m->b, m->c, m->d, m->wk, m->m, m->pj, m->br, m->n);
    idx = (idx + 1) % (sizeof(states)/sizeof(states[0]));
}

static void heap_refresh_cb(lv_timer_t *t) {
    (void)t;
    if (!set_heap) return;
    size_t free = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    char buf[32]; snprintf(buf, sizeof(buf), "%u KB free", (unsigned)(free / 1024));
    lv_label_set_text(set_heap, buf);
}

// ============================================================================
// Boot
// ============================================================================

void clawdmeter_build_screens(void) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    tileview = lv_tileview_create(scr);
    lv_obj_set_size(tileview, CANVAS_W, CANVAS_H);
    lv_obj_set_pos(tileview, 0, 0);
    lv_obj_set_style_bg_opa(tileview, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tileview, 0, 0);
    lv_obj_set_style_pad_all(tileview, 0, 0);
    lv_obj_set_scrollbar_mode(tileview, LV_SCROLLBAR_MODE_OFF);

    tile_usage    = lv_tileview_add_tile(tileview, 0, 0, LV_DIR_HOR);
    tile_sessions = lv_tileview_add_tile(tileview, 1, 0, LV_DIR_HOR);
    tile_tokens   = lv_tileview_add_tile(tileview, 2, 0, LV_DIR_HOR);
    tile_settings = lv_tileview_add_tile(tileview, 3, 0, LV_DIR_HOR);

    build_usage_tile   (tile_usage);
    build_sessions_tile(tile_sessions);
    build_tokens_tile  (tile_tokens);
    build_settings_tile(tile_settings);

    // Attention overlay sits on the active screen, above the tileview.
    build_attention_overlay(scr);

    if (set_version) lv_label_set_text(set_version, "3.49-0.2");

    // Initial empty sessions list
    clawdmeter_apply_sessions(NULL, 0);

    // Initial mock state - suppressed once real BLE data lands.
    update_state(47, 31, 89, 8640, 82, 1, 142, 412, 2840, 410, 4245, 3);
    mock_timer = lv_timer_create(mock_cycle_cb, 3000, NULL);
    lv_timer_create(heap_refresh_cb, 2000, NULL);
}
