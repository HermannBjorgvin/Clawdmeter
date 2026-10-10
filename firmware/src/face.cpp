#include "face.h"
#include "theme.h"
#include "hal/board_caps.h"

// Geometry, as fractions of the panel width (square and portrait panels alike).
// Eyes are tall black rectangles; the official still pose puts them at ~19% /
// 81% across the head, pulled in here to clear the panel's rounded corners.
#define EYE_W_DIV        8      // eye width  = W / 8  (60 px on 480)
#define EYE_H_DIV        5      // eye height = W / 5  (96 px on 480)
#define EYE_CX_LEFT_PCT  25     // eye centers, % of width
#define EYE_CX_RIGHT_PCT 75
#define EYE_CY_PCT       44     // % of height — a touch above middle

// Blink timing.
#define BLINK_GAP_MIN_MS   2000
#define BLINK_GAP_MAX_MS   6000
#define BLINK_SHUT_MS       180   // how long the "> <" squint holds
#define BLINK_REOPEN_MS     140   // open gap inside a double blink
#define DOUBLE_BLINK_PCT     20

// Glances: both eyes slide a little left or right, hold, then come back.
#define GLANCE_SHIFT_DIV     24   // shift = W / 24 (20 px on 480)
#define GLANCE_GAP_MIN_MS  3000
#define GLANCE_GAP_MAX_MS  8000
#define GLANCE_HOLD_MIN_MS  700
#define GLANCE_HOLD_MAX_MS 1800
#define GLANCE_MOVE_MS      140   // eased slide between positions
#define BOTH_WAYS_PCT        25   // look one way, then the other, before centering
#define GLANCE_TRAIL_PCT    150   // the eye away from the look travels 1.5x, so
                                  // the eyes draw closer together mid-glance

static lv_obj_t *root = NULL;
static lv_obj_t *eye[2] = {NULL, NULL};
static lv_obj_t *squint[2] = {NULL, NULL};
static lv_timer_t *blink_timer = NULL;
static uint8_t blinks_left = 0;               // squints still to do this blink
static lv_timer_t *glance_timer = NULL;
static int32_t gaze_x = 0;                    // current eye offset, px
static int32_t glance_px = 20;                // shift distance (set from W)
static int8_t  glance_dir = 0;                // -1 left, +1 right, 0 centered
static bool    glance_other_way = false;      // pending "now look the other way"

static void set_shut(bool shut) {
    for (int i = 0; i < 2; i++) {
        if (shut) { lv_obj_add_flag(eye[i], LV_OBJ_FLAG_HIDDEN);   lv_obj_clear_flag(squint[i], LV_OBJ_FLAG_HIDDEN); }
        else      { lv_obj_clear_flag(eye[i], LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(squint[i], LV_OBJ_FLAG_HIDDEN); }
    }
}

// Shift both eyes (and their squints, so a blink mid-glance stays put) via
// translate_x — a pure render offset, no re-layout. `x` is the leading eye's
// offset; the trailing eye (the one on the far side of the look) moves
// GLANCE_TRAIL_PCT of it, so the gap narrows as he looks and widens again as
// he comes back. Scaling with x keeps it continuous through center.
static void gaze_apply(void *var, int32_t x) {
    (void)var;
    gaze_x = x;
    for (int i = 0; i < 2; i++) {
        const bool leading = (x < 0) == (i == 0);   // left eye leads a left look
        const int32_t dx = leading ? x : x * GLANCE_TRAIL_PCT / 100;
        lv_obj_set_style_translate_x(eye[i], dx, 0);
        lv_obj_set_style_translate_x(squint[i], dx, 0);
    }
}

static void gaze_to(int32_t x) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, root);
    lv_anim_set_exec_cb(&a, gaze_apply);
    lv_anim_set_values(&a, gaze_x, x);
    lv_anim_set_duration(&a, GLANCE_MOVE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

// centered → (random gap) → glance one way → (hold) → [sometimes the other
// way → (hold)] → centered.
static void glance_cb(lv_timer_t *t) {
    if (glance_dir == 0) {
        glance_dir = lv_rand(0, 1) ? 1 : -1;
        glance_other_way = lv_rand(0, 99) < BOTH_WAYS_PCT;
        gaze_to(glance_dir * glance_px);
        lv_timer_set_period(t, lv_rand(GLANCE_HOLD_MIN_MS, GLANCE_HOLD_MAX_MS));
    } else if (glance_other_way) {
        glance_other_way = false;
        glance_dir = -glance_dir;
        gaze_to(glance_dir * glance_px);
        lv_timer_set_period(t, lv_rand(GLANCE_HOLD_MIN_MS, GLANCE_HOLD_MAX_MS));
    } else {
        glance_dir = 0;
        gaze_to(0);
        lv_timer_set_period(t, lv_rand(GLANCE_GAP_MIN_MS, GLANCE_GAP_MAX_MS));
    }
}

// One timer drives the whole cycle: open → (random gap) → shut → open, with a
// second shut/open pass now and then for a double blink.
static void blink_cb(lv_timer_t *t) {
    const bool shut = lv_obj_has_flag(eye[0], LV_OBJ_FLAG_HIDDEN);
    if (!shut) {
        if (blinks_left == 0)
            blinks_left = (lv_rand(0, 99) < DOUBLE_BLINK_PCT) ? 2 : 1;
        set_shut(true);
        lv_timer_set_period(t, BLINK_SHUT_MS);
        return;
    }
    set_shut(false);
    blinks_left--;
    lv_timer_set_period(t, blinks_left ? BLINK_REOPEN_MS
                                       : lv_rand(BLINK_GAP_MIN_MS, BLINK_GAP_MAX_MS));
}

lv_obj_t* face_init(lv_obj_t *parent) {
    const BoardCaps& c = board_caps();
    const int W = c.width, H = c.height;
    const int ew = W / EYE_W_DIV, eh = W / EYE_H_DIV;
    const int cy = H * EYE_CY_PCT / 100;
    glance_px = W / GLANCE_SHIFT_DIV;

    root = lv_obj_create(parent);
    lv_obj_set_size(root, W, H);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, THEME_ACCENT, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 2; i++) {
        const int cx = W * (i == 0 ? EYE_CX_LEFT_PCT : EYE_CX_RIGHT_PCT) / 100;
        const int x0 = cx - ew / 2, y0 = cy - eh / 2;

        eye[i] = lv_obj_create(root);
        lv_obj_set_size(eye[i], ew, eh);
        lv_obj_set_pos(eye[i], x0, y0);
        lv_obj_set_style_bg_color(eye[i], lv_color_black(), 0);
        lv_obj_set_style_bg_opa(eye[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(eye[i], 0, 0);
        lv_obj_set_style_radius(eye[i], 0, 0);
        lv_obj_clear_flag(eye[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(eye[i], LV_OBJ_FLAG_EVENT_BUBBLE);

        // Squint in the same box, drawn as pixel art like the Clawd sprites:
        // a 4x5 grid, two solid cells per row stepping in then out — ">" on
        // the left eye, "<" on the right — so the tips stay crisp (thick
        // lv_lines notch at the joint) and the stroke reads as chunky as the eye.
        squint[i] = lv_obj_create(root);
        lv_obj_set_size(squint[i], ew, eh);
        lv_obj_set_pos(squint[i], x0, y0);
        lv_obj_set_style_bg_opa(squint[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(squint[i], 0, 0);
        lv_obj_set_style_pad_all(squint[i], 0, 0);
        lv_obj_clear_flag(squint[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(squint[i], LV_OBJ_FLAG_EVENT_BUBBLE);
        static const uint8_t CHEVRON_COL[5] = {0, 1, 2, 1, 0};   // ">" left cell per row
        for (int r = 0; r < 5; r++) {
            const int col = (i == 0) ? CHEVRON_COL[r] : 2 - CHEVRON_COL[r];
            const int cx0 = ew * col / 4,  cx1 = ew * (col + 2) / 4;
            const int cy0 = eh * r / 5,    cy1 = eh * (r + 1) / 5;
            lv_obj_t* cell = lv_obj_create(squint[i]);
            lv_obj_set_size(cell, cx1 - cx0, cy1 - cy0);
            lv_obj_set_pos(cell, cx0, cy0);
            lv_obj_set_style_bg_color(cell, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(cell, 0, 0);
            lv_obj_set_style_radius(cell, 0, 0);
            lv_obj_set_style_pad_all(cell, 0, 0);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(cell, LV_OBJ_FLAG_EVENT_BUBBLE);
        }
        lv_obj_add_flag(squint[i], LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    return root;
}

void face_show(void) {
    if (!root) return;
    set_shut(false);
    blinks_left = 0;
    lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(root);
    lv_rand_set_seed(lv_tick_get());   // vary the blink rhythm per visit
    if (!blink_timer)
        blink_timer = lv_timer_create(blink_cb, lv_rand(BLINK_GAP_MIN_MS, BLINK_GAP_MAX_MS), NULL);
    glance_dir = 0;
    glance_other_way = false;
    gaze_apply(NULL, 0);
    if (!glance_timer)
        glance_timer = lv_timer_create(glance_cb, lv_rand(GLANCE_GAP_MIN_MS, GLANCE_GAP_MAX_MS), NULL);
}

void face_hide(void) {
    if (!root) return;
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    if (blink_timer)  { lv_timer_delete(blink_timer);  blink_timer = NULL; }
    if (glance_timer) { lv_timer_delete(glance_timer); glance_timer = NULL; }
    lv_anim_delete(root, gaze_apply);
    gaze_apply(NULL, 0);
}

lv_obj_t* face_get_root(void) { return root; }
