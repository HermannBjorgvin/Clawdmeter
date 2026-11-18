#ifdef PET_ASCII
// The pet up close: face.h for an ASCII pet instead of Clawd's face (this
// file replaces face.cpp in a -DPET_ASCII build).
//
// A black full-screen overlay on lv_layer_top() with the pet drawn large. It
// slides up from the bottom edge and back down, where Clawd's face rises and
// drops. Two uses, as with the face:
//   screensaver (face_rise):        the pet alone, asleep; the time it rests
//                                   is its nap (pet_on_nap_end)
//   peek (face_rise_toward, a tap): the pet awake, with its stats under it;
//                                   taps at the sides change the species
#include "../face.h"
#include "../theme.h"
#include "../hal/board_caps.h"
#include "pet_buddy.h"
#include "pet_face.h"
#include "pet_hooks.h"
#include "pet_stats_core.h"
#include "pet_view.h"
#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>

LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_14);

#define RISE_MS   420
#define DROP_MS   320
#define BLINK_MS  260

static lv_obj_t* root = NULL;
static PetView   view;
static bool      have_view = false;
static int       W = 480, H = 480;

// Stats, shown on the peek only.
enum { ROW_MOOD, ROW_FED, ROW_ENERGY, ROW_COUNT };
static const uint8_t     ROW_PIPS[ROW_COUNT]  = { PET_MOOD_MAX, PET_FED_PIPS, PET_ENERGY_MAX };
static const char* const ROW_NAMES[ROW_COUNT] = { "mood", "fed", "energy" };
static lv_obj_t* stats_box = NULL;
static lv_obj_t* lbl_title = NULL;
static lv_obj_t* lbl_row[ROW_COUNT];
static lv_obj_t* pips[ROW_COUNT][PET_FED_PIPS];
static lv_obj_t* lbl_prev = NULL;
static lv_obj_t* lbl_next = NULL;
static int       shown_vals[ROW_COUNT] = { -1, -1, -1 };
static int       shown_level = -1, shown_species = -1;

static bool      visible = false;
static bool      moving = false;
static bool      dropping = false;
static bool      resting = false;
static bool      peek = false;
static uint32_t  blink_until = 0;       // 0 = not blinking
static uint32_t  look_until = 0;        // 0 = not looking at a tap
static bool      napping = false;
static uint32_t  nap_since = 0;
static uint8_t   drawn_state = 0xFF;
static uint32_t  drawn_tick = 0;
static int       anim_var;              // lv_anim's handle for the slide

static lv_color_t row_color(int row) {
    switch (row) {
    case ROW_MOOD:   return lv_color_hex(0xFF2D7A);   // the pets' heart pink
    case ROW_FED:    return THEME_AMBER;
    default:         return THEME_GREEN;
    }
}

// ─── What the big pet is doing ───────────────────────────────────────────────
static uint8_t state_now(void) {
    if (peek) {
        // Pleased to see you while he looks at your tap and when he blinks
        // at it; after that, whatever he'd be doing in his seat.
        if (look_until || blink_until) return PET_HEART;
        return resting ? pet_persona() : PET_HEART;
    }
    // The screensaver: asleep, except the moment he notices you (the blink
    // the director asks for before the drop).
    return blink_until ? PET_IDLE : PET_SLEEP;
}

static void draw(void) {
    if (!have_view) return;
    drawn_state = state_now();
    drawn_tick = pet_ticks();
    pet_view_draw(&view, drawn_state);
}

static void end_nap(void) {
    if (!napping) return;
    napping = false;
    pet_on_nap_end((uint32_t)(millis() - nap_since) / 1000u);
}

// ─── Stats ───────────────────────────────────────────────────────────────────
static void refresh_stats(bool force) {
    if (!stats_box) return;
    const int level = pet_level(), species = pet_species_idx();
    if (force || level != shown_level || species != shown_species) {
        shown_level = level;
        shown_species = species;
        char buf[40];
        snprintf(buf, sizeof(buf), "%s  -  level %d", pet_species_name(), level);
        lv_label_set_text(lbl_title, buf);
    }
    const int vals[ROW_COUNT] = { pet_mood(), pet_fed(), pet_energy() };
    for (int r = 0; r < ROW_COUNT; r++) {
        if (!force && vals[r] == shown_vals[r]) continue;
        shown_vals[r] = vals[r];
        for (int i = 0; i < ROW_PIPS[r]; i++)
            lv_obj_set_style_bg_color(pips[r][i], i < vals[r] ? row_color(r) : THEME_BAR_BG, 0);
    }
}

static lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, lv_color_t color) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static void build_stats(int top) {
    const int mind = W < H ? W : H;
    const lv_font_t* font = mind >= 400 ? &font_styrene_20 : &font_styrene_14;
    const int rh = mind / 12;                     // row pitch
    const int pip = rh / 2, gap = pip / 3 > 0 ? pip / 3 : 1;
    const int label_x = W * 14 / 100, pips_x = W * 40 / 100;

    stats_box = lv_obj_create(root);
    lv_obj_remove_style_all(stats_box);
    lv_obj_set_pos(stats_box, 0, top);
    lv_obj_set_size(stats_box, W, H - top);
    lv_obj_clear_flag(stats_box, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    lbl_title = make_label(stats_box, font, THEME_TEXT);
    lv_obj_set_width(lbl_title, W);
    lv_obj_set_style_text_align(lbl_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(lbl_title, 0, 0);

    for (int r = 0; r < ROW_COUNT; r++) {
        const int y = rh * (r + 1) + rh / 4;
        lbl_row[r] = make_label(stats_box, font, THEME_DIM);
        lv_label_set_text(lbl_row[r], ROW_NAMES[r]);
        lv_obj_set_pos(lbl_row[r], label_x, y - (lv_font_get_line_height(font) - pip) / 2);
        for (int i = 0; i < ROW_PIPS[r]; i++) {
            lv_obj_t* p = lv_obj_create(stats_box);
            lv_obj_remove_style_all(p);
            lv_obj_set_pos(p, pips_x + i * (pip + gap), y);
            lv_obj_set_size(p, pip, pip);
            lv_obj_set_style_radius(p, pip / 4, 0);
            lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(p, THEME_BAR_BG, 0);
            lv_obj_clear_flag(p, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
            pips[r][i] = p;
        }
    }

    // Where a tap changes the species.
    lbl_prev = make_label(stats_box, font, THEME_DIM);
    lv_label_set_text(lbl_prev, "<");
    lv_obj_set_pos(lbl_prev, W * 4 / 100, 0);
    lbl_next = make_label(stats_box, font, THEME_DIM);
    lv_label_set_text(lbl_next, ">");
    lv_obj_set_pos(lbl_next, W - W * 4 / 100 - lv_font_get_glyph_width(font, '>', 0), 0);
}

// Peek: the pet at the top with its stats under it. Screensaver: the pet
// alone, its body centred on the screen.
static void layout_for(bool with_stats) {
    if (!have_view) return;
    const int s = view.scale;
    const int x = (W - pet_view_w(&view)) / 2;
    const int body_mid = (PET_BODY_TOP + PET_FEET_Y) / 2 * s;
    const int stats_top = PET_FEET_Y * s + H / 30;
    const bool room = stats_box && stats_top + (H / 12) * (ROW_COUNT + 1) <= H;
    if (with_stats && room) {
        lv_obj_set_pos(view.img, x, 0);
        lv_obj_set_y(stats_box, stats_top);
        lv_obj_clear_flag(stats_box, LV_OBJ_FLAG_HIDDEN);
        refresh_stats(true);
    } else {
        lv_obj_set_pos(view.img, x, H / 2 - body_mid);
        if (stats_box) lv_obj_add_flag(stats_box, LV_OBJ_FLAG_HIDDEN);
    }
}

// ─── The slide ───────────────────────────────────────────────────────────────
static void hide_now(void) {
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    visible = false;
    moving = false;
    dropping = false;
}

static void slide_exec(void* var, int32_t v) {
    (void)var;
    lv_obj_set_y(root, v);
}

static void slide_done(lv_anim_t* a) {
    (void)a;
    moving = false;
    if (dropping) hide_now();
}

static void slide(bool up) {
    lv_anim_delete(&anim_var, NULL);
    const int from = visible ? lv_obj_get_y(root) : H;
    if (!visible) {
        lv_obj_set_y(root, H);
        lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(root);
    }
    visible = true;
    moving = true;
    dropping = !up;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &anim_var);
    lv_anim_set_exec_cb(&a, slide_exec);
    lv_anim_set_values(&a, from, up ? 0 : H);
    lv_anim_set_duration(&a, up ? RISE_MS : DROP_MS);
    lv_anim_set_path_cb(&a, up ? lv_anim_path_ease_out : lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a, slide_done);
    lv_anim_start(&a);
}

static void rise(bool as_peek) {
    if (!root) return;
    peek = as_peek;
    resting = false;
    blink_until = 0;
    look_until = 0;
    layout_for(as_peek);
    pet_view_show(&view, true);
    draw();
    slide(true);
}

// ─── API (face.h) ────────────────────────────────────────────────────────────
void face_init(void) {
    const BoardCaps& c = board_caps();
    W = c.width;
    H = c.height;
    root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, W, H);
    lv_obj_set_pos(root, 0, H);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);

    // As large as fits the width, leaving the lower part of the screen for
    // the stats (4x on a 480 panel).
    int scale = W / PET_W;
    const int by_h = H * 72 / 100 / PET_H;
    if (by_h < scale) scale = by_h;
    if (scale < 1) scale = 1;
    have_view = pet_view_create(&view, root, scale, false);
    if (have_view) build_stats(PET_FEET_Y * scale + H / 30);
}

void face_tick(void) {
    if (!root || !visible) return;
    pet_clock();
    const uint32_t now = millis();
    if (blink_until && (int32_t)(now - blink_until) >= 0) blink_until = 0;
    if (look_until && (int32_t)(now - look_until) >= 0) look_until = 0;
    if (pet_ticks() != drawn_tick || state_now() != drawn_state) {
        draw();
        if (peek) refresh_stats(false);
    }
}

void face_rise(void)           { rise(false); }
void face_rise_toward(int x)   { (void)x; rise(true); }

void face_drop(void) {
    if (!root || !visible) return;
    resting = false;
    end_nap();
    blink_until = 0;
    look_until = 0;
    slide(false);
}

void face_hide_now(void) {
    if (!root) return;
    lv_anim_delete(&anim_var, NULL);
    resting = false;
    end_nap();
    blink_until = 0;
    look_until = 0;
    lv_obj_set_y(root, H);
    hide_now();
}

bool face_visible(void) { return visible; }
bool face_moving(void)  { return moving; }
bool face_busy(void)    { return moving || blink_until != 0 || look_until != 0; }

void face_blink(bool twice) {
    if (!visible) return;
    blink_until = millis() + (twice ? 2 * BLINK_MS : BLINK_MS);
    if (blink_until == 0) blink_until = 1;
}

void face_set_resting(bool on) {
    resting = on;
    if (on && !peek && !napping) {
        napping = true;
        nap_since = millis();
    } else if (!on) {
        end_nap();
    }
}

void face_apply_theme(void) {
    if (!stats_box) return;
    lv_obj_set_style_text_color(lbl_title, THEME_TEXT, 0);
    lv_obj_set_style_text_color(lbl_prev, THEME_DIM, 0);
    lv_obj_set_style_text_color(lbl_next, THEME_DIM, 0);
    for (int r = 0; r < ROW_COUNT; r++) lv_obj_set_style_text_color(lbl_row[r], THEME_DIM, 0);
    refresh_stats(true);
}

void face_look_at_x(int x, uint32_t hold_ms) {
    (void)x;
    if (!visible) return;
    look_until = millis() + (hold_ms ? hold_ms : 1);
    if (look_until == 0) look_until = 1;
}

// ─── Species choice (pet_face.h) ─────────────────────────────────────────────
bool pet_face_tap(int x) {
    if (!visible || !peek || dropping) return false;
    int dir = 0;
    if (x < W / 3)            dir = -1;
    else if (x >= W - W / 3)  dir = +1;
    if (dir == 0) return false;
    pet_species_step(dir);
    draw();
    refresh_stats(false);
    return true;
}

#endif  // PET_ASCII
