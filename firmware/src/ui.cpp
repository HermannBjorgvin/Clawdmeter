#include "ui.h"
#include <lvgl.h>
#include "logo.h"

// Custom fonts
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_styrene_12);
LV_FONT_DECLARE(font_mono_18);

// Anthropic brand palette
#define COL_BG        lv_color_hex(0x141413)
#define COL_PANEL     lv_color_hex(0x1f1f1e)
#define COL_TEXT      lv_color_hex(0xfaf9f5)
#define COL_DIM       lv_color_hex(0xb0aea5)
#define COL_ACCENT    lv_color_hex(0xd97757)
#define COL_GREEN     lv_color_hex(0x788c5d)
#define COL_AMBER     lv_color_hex(0xd97757)
#define COL_RED       lv_color_hex(0xc0392b)
#define COL_BAR_BG    lv_color_hex(0x2a2a28)

// Widget handles
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

// Logo image descriptor for LVGL
static lv_image_dsc_t logo_dsc;

// Animation state
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_msg_idx = 0;
static uint32_t anim_msg_start = 0;
#define ANIM_SPIN_MS    120   // spinner frame speed
#define ANIM_MSG_MS     4000  // how long each message stays

// Claude's spinner characters: · ✻ ✽ ✶ ✳ ✢
// UTF-8 encoded
static const char* const spinner_frames[] = {
    "\xC2\xB7",         // · (U+00B7)
    "\xE2\x9C\xBB",     // ✻ (U+273B)
    "\xE2\x9C\xBD",     // ✽ (U+273D)
    "\xE2\x9C\xB6",     // ✶ (U+2736)
    "\xE2\x9C\xB3",     // ✳ (U+2733)
    "\xE2\x9C\xA2",     // ✢ (U+2722)
};
#define SPINNER_COUNT 6

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

static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, 12, 0);
    lv_obj_set_style_pad_right(panel, 12, 0);
    lv_obj_set_style_pad_top(panel, 10, 0);
    lv_obj_set_style_pad_bottom(panel, 16, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
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

void ui_init(void) {
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // ---- Logo image ----
    logo_dsc.header.w = LOGO_WIDTH;
    logo_dsc.header.h = LOGO_HEIGHT;
    logo_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    logo_dsc.header.stride = LOGO_WIDTH * 2;
    logo_dsc.data = (const uint8_t*)logo_data;
    logo_dsc.data_size = LOGO_WIDTH * LOGO_HEIGHT * 2;

    lv_obj_t* img = lv_image_create(scr);
    lv_image_set_src(img, &logo_dsc);
    lv_obj_set_pos(img, 10, 4);

    // ---- Title (vertically centered to 48px logo) ----
    lbl_title = lv_label_create(scr);
    lv_label_set_text(lbl_title, "Claude Usage Tracker");
    lv_obj_set_style_text_font(lbl_title, &font_tiempos_34, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    lv_obj_set_pos(lbl_title, 66, 12);

    // ---- Session panel ----
    // Layout: percentage -> bar -> "5-Hour Session ... Resets in ..." below
    lv_obj_t* p_session = make_panel(scr, 8, 56, 464, 100);

    lbl_session_pct = lv_label_create(p_session);
    lv_label_set_text(lbl_session_pct, "---%");
    lv_obj_set_style_text_font(lbl_session_pct, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_session_pct, COL_TEXT, 0);
    lv_obj_set_pos(lbl_session_pct, 0, 0);

    bar_session = make_bar(p_session, 0, 36, 440, 20);

    lbl_session_label = lv_label_create(p_session);
    lv_label_set_text(lbl_session_label, "Current Session");
    lv_obj_set_style_text_font(lbl_session_label, &font_styrene_16, 0);
    lv_obj_set_style_text_color(lbl_session_label, COL_DIM, 0);
    lv_obj_set_pos(lbl_session_label, 0, 62);

    lbl_session_reset = lv_label_create(p_session);
    lv_label_set_text(lbl_session_reset, "---");
    lv_obj_set_style_text_font(lbl_session_reset, &font_styrene_16, 0);
    lv_obj_set_style_text_color(lbl_session_reset, COL_DIM, 0);
    lv_obj_set_pos(lbl_session_reset, 440, 62);
    lv_obj_set_style_text_align(lbl_session_reset, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(lbl_session_reset, LV_ALIGN_TOP_RIGHT, 0, 62);

    // ---- Weekly panel ----
    lv_obj_t* p_weekly = make_panel(scr, 8, 166, 464, 100);

    lbl_weekly_pct = lv_label_create(p_weekly);
    lv_label_set_text(lbl_weekly_pct, "---%");
    lv_obj_set_style_text_font(lbl_weekly_pct, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_weekly_pct, COL_TEXT, 0);
    lv_obj_set_pos(lbl_weekly_pct, 0, 0);

    bar_weekly = make_bar(p_weekly, 0, 36, 440, 20);

    lbl_weekly_label = lv_label_create(p_weekly);
    lv_label_set_text(lbl_weekly_label, "Current Week");
    lv_obj_set_style_text_font(lbl_weekly_label, &font_styrene_16, 0);
    lv_obj_set_style_text_color(lbl_weekly_label, COL_DIM, 0);
    lv_obj_set_pos(lbl_weekly_label, 0, 62);

    lbl_weekly_reset = lv_label_create(p_weekly);
    lv_label_set_text(lbl_weekly_reset, "---");
    lv_obj_set_style_text_font(lbl_weekly_reset, &font_styrene_16, 0);
    lv_obj_set_style_text_color(lbl_weekly_reset, COL_DIM, 0);
    lv_obj_align(lbl_weekly_reset, LV_ALIGN_TOP_RIGHT, 0, 62);

    // ---- Animation at bottom ----
    lbl_anim = lv_label_create(scr);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, &font_mono_18, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -16);
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
    uint32_t now = lv_tick_get();

    // Switch to next message
    if (now - anim_msg_start >= ANIM_MSG_MS) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_msg_start = now;
    }

    // Spin the spinner
    if (now - anim_last_ms >= ANIM_SPIN_MS) {
        anim_last_ms = now;
        anim_spinner_idx = (anim_spinner_idx + 1) % SPINNER_COUNT;

        static char buf[80];
        snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
                 spinner_frames[anim_spinner_idx],
                 anim_messages[anim_msg_idx]);
        lv_label_set_text(lbl_anim, buf);
    }
}

void ui_set_status(const char* text) {
    (void)text;
}
