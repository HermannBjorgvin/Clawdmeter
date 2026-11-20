#include "ota_ui.h"

#include <stdio.h>
#include <lvgl.h>

#include "hal/board_caps.h"
#include "layout.h"
#include "minimal_mode.h"
#include "ota_proto.h"
#include "theme.h"

static lv_obj_t* panel = nullptr;
static lv_obj_t* title = nullptr;
static lv_obj_t* pct_label = nullptr;
static lv_obj_t* bar = nullptr;
static lv_obj_t* note = nullptr;
static int shown_phase = -1;   // none yet
static int shown_pct = -1;
static int last_pct = 0;
static int16_t bar_w = 0, bar_h = 0;
static bool tapped = false;

static void on_click(lv_event_t* e) {
    (void)e;
    if (shown_phase == OTA_UI_CONFIRM) tapped = true;
}

static lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, lv_color_t color, int16_t w) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, w);
    return l;
}

// Built on first use, so it lands above the screensaver face (also on the
// top layer) and costs nothing on a board that never updates.
static void build(void) {
    const BoardCaps& caps = board_caps();
    const Layout L = layout_compute({ caps.width, caps.height, caps.round_panel });
    panel = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, caps.width, caps.height);
    lv_obj_set_style_bg_color(panel, THEME_BG, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    // Taps land here, not on the meters or Clawd underneath; on the
    // confirmation they allow the update.
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(panel, on_click, LV_EVENT_CLICKED, nullptr);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    // The pairing hint's width already keeps inside a round panel. The
    // percentage is the big number (the meters' own); the heading takes the
    // largest of the hint's smaller sizes that fits on one line (it wraps
    // only on the narrowest panels), the note the smallest.
    const int16_t w = L.pair_w;
    static const char* const TITLE = "Updating firmware";
    const lv_font_t* title_font = L.pair_font[3];
    for (int i = 1; i <= 3; i++) {
        if (layout_text_width(L.pair_font[i], TITLE) <= w) {
            title_font = L.pair_font[i];
            break;
        }
    }
    title = make_label(panel, title_font, THEME_TEXT, w);
    lv_label_set_text(title, TITLE);
    pct_label = make_label(panel, L.pct_font, THEME_TEXT, w);
    note = make_label(panel, L.pair_font[3], THEME_DIM, w);

    bar_w = (int16_t)(w * 4 / 5);
    bar_h = (int16_t)(caps.height >= 300 ? 10 : caps.mono_panel ? 8 : 6);
    bar = lv_bar_create(panel);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, bar_w, bar_h);
    lv_obj_set_style_bg_color(bar, THEME_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, bar_h / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, THEME_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, bar_h / 2, LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, 100);
    if (caps.mono_panel) {   // the dark track prints as paper: outline it, as the meters do
        lv_obj_set_style_border_color(bar, THEME_TEXT, LV_PART_MAIN);
        lv_obj_set_style_border_width(bar, 2, LV_PART_MAIN);
    }
}

// Stack the four parts in the middle of the panel.
static void place(void) {
    const BoardCaps& caps = board_caps();
    lv_obj_update_layout(panel);
    const int16_t gap = (int16_t)(caps.height / 24);
    const int16_t th = (int16_t)lv_obj_get_height(title);
    const int16_t ph = (int16_t)lv_obj_get_height(pct_label);
    const int16_t nh = (int16_t)lv_obj_get_height(note);
    const int16_t total = th + gap + ph + gap + bar_h + gap + nh;
    int16_t y = (int16_t)((caps.height - total) / 2);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, y);
    y += th + gap;
    lv_obj_align(pct_label, LV_ALIGN_TOP_MID, 0, y);
    y += ph + gap;
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, y);
    y += bar_h + gap;
    lv_obj_align(note, LV_ALIGN_TOP_MID, 0, y);
}

void ota_ui_show(OtaUiPhase phase, int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    // A slow panel (e-paper) redraws in tenths, not every percent, and
    // counts down in fives.
    if (minimal_mode() && phase == OTA_UI_RECEIVING) pct -= pct % 10;
    if (minimal_mode() && phase == OTA_UI_CONFIRM) pct = (pct + 4) / 5 * 5;
    if (!panel) build();
    if (lv_obj_has_flag(panel, LV_OBJ_FLAG_HIDDEN) || (int)phase != shown_phase) {
        lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(panel);
        // Read the palette now: the app may have changed it since.
        lv_obj_set_style_bg_color(panel, THEME_BG, 0);
        lv_obj_set_style_text_color(title, THEME_TEXT, 0);
        lv_obj_set_style_text_color(pct_label, THEME_TEXT, 0);
        lv_obj_set_style_text_color(note, phase == OTA_UI_FAILED ? THEME_RED : THEME_DIM, 0);
        lv_obj_set_style_bg_color(bar, THEME_BAR_BG, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, phase == OTA_UI_FAILED ? THEME_RED : THEME_ACCENT, LV_PART_INDICATOR);
        lv_label_set_text(title, phase == OTA_UI_CONFIRM ? "Update firmware?" : "Updating firmware");
        const char* text = "";
        switch (phase) {
        case OTA_UI_CONFIRM:
            text = board_caps().has_touch ? "Tap the screen to allow" : "Press a button twice to allow";
            tapped = false;   // only taps on this screen count
            last_pct = 0;     // a refusal shows an empty bar, not the last update's
            break;
        case OTA_UI_RECEIVING:  text = "Keep the board on"; break;
        case OTA_UI_VERIFYING:  text = "Checking the firmware..."; break;
        case OTA_UI_RESTARTING: text = "Restarting..."; break;
        case OTA_UI_FAILED:     text = "Update stopped"; break;
        }
        lv_label_set_text(note, text);
        shown_phase = phase;
        shown_pct = -1;
    }
    char buf[12];
    int show, fill;
    if (phase == OTA_UI_CONFIRM) {
        // The seconds left, and the bar running down with them.
        show = pct;
        fill = pct * 100 / OTA_CONFIRM_SECS;
        snprintf(buf, sizeof(buf), "%d s", show);
    } else {
        // A failed update keeps the bar where it stopped.
        if (phase == OTA_UI_RECEIVING) last_pct = pct;
        show = last_pct;
        if (phase == OTA_UI_VERIFYING || phase == OTA_UI_RESTARTING) show = 100;
        fill = show;
        snprintf(buf, sizeof(buf), "%d%%", show);
    }
    if (show != shown_pct) {
        lv_label_set_text(pct_label, buf);
        lv_bar_set_value(bar, fill > 100 ? 100 : fill, LV_ANIM_OFF);
        shown_pct = show;
        place();
    }
}

bool ota_ui_take_tap(void) {
    const bool t = tapped;
    tapped = false;
    return t;
}

void ota_ui_hide(void) {
    if (panel) lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
}

bool ota_ui_visible(void) {
    return panel && !lv_obj_has_flag(panel, LV_OBJ_FLAG_HIDDEN);
}
