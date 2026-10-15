#include "face.h"
#include "clawd_policy.h"
#include "theme.h"
#include "perf.h"
#include "field.h"
#include "face_motion.h"
#include "hal/board_caps.h"
#include <lvgl.h>
#include <Arduino.h>

// Geometry is authored on a 480 px square (design system storyboards) and
// scaled by the panel's shorter side; the eye block is centred vertically.
//   eyes: 44 x 120 px, centres 160 px apart, top at y 150
//   arms (only while moving): 48 x 56 px nubs, top at y 240
//   squint: u = 12 px cells, a 5-row chevron per eye (> <)
//   glance: 20 px, far eye 1.5x
// The rise and the drop are face_motion.cpp: straight up and down with a flat
// top edge, arms on the rise, squash and stretch on the eyes, follow-through
// (docs/clawd-director.md, "The face's motion").
#define BLINK_MS   180
#define BLINK_GAP  140
#define GLANCE_MS  280

#define COL_FACE   THEME_ACCENT               // Clawd orange
#define COL_EYE    lv_color_hex(0x141413)

static Field     field;      // the body + arms: redraws only what changes
static lv_obj_t* root = NULL; // == field.obj: full screen, never moves; the eyes are its children
static lv_obj_t* eye_box[2];
static lv_obj_t* eye_open[2];
static lv_obj_t* squint[2][9];

static int W = 480, H = 480, mind = 480;
static int eye_cx[2];        // eye centre x, px
static int eye_top = 150;    // eye block top, px (below the body's top edge)
static int eye_w0 = 44, eye_h0 = 120;
static int box_w0 = 52, box_h0 = 120;
static int arm_top = 240;    // arm nub top, px (below the body's top edge)
static int glance_px = 20;

static FaceGeom geom;
static FacePose pose;        // what's on screen
static FaceMove move_kind = FACE_MOVE_RISE;
static bool     settling = false;   // the rise reached rest; perf's settle window is open
static int      anim_var;           // lv_anim's handle for the motion

static bool moving = false;
static bool visible = false;
static bool resting = false;

// blink: 0 none, 1 closed, 2 open gap (double), 3 closed again
static uint8_t  blink_phase = 0;
static uint32_t blink_until = 0;
static bool     blink_twice = false;
// glance: 0 none, 1 out (anim), 2 hold, 3 back (anim)
static uint8_t  glance_phase = 0;
static uint32_t glance_until = 0;
static int      glance_target = 0;
static int      glance_cur = 0;
static uint32_t glance_hold = 0;      // 0: a resting glance's random hold
static uint32_t next_act_at = 0;
static int      interest_g = 0;       // curious about a tap: resting glances go back there (px)
static ClawdRng rng;

// Last geometry handed to LVGL per eye, so an unchanged eye costs nothing.
struct EyeBox { int x, y, w, h, ew, eh; bool solid; };   // solid: the box is exactly the open eye
static EyeBox eye_last[2];
static bool   squinting = false;

static int S(int v) { return v * mind / 480; }

static void apply_eyes(void);

static void set_squint(bool on) {
    if (on != squinting) {
        // Squinting: the box widens to hold the chevron, sized before the
        // chevron shows so it never draws clipped.
        squinting = on;
        apply_eyes();
    }
    for (int e = 0; e < 2; e++) {
        if (on) lv_obj_add_flag(eye_open[e], LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_clear_flag(eye_open[e], LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 9; i++) {
            if (on) lv_obj_clear_flag(squint[e][i], LV_OBJ_FLAG_HIDDEN);
            else    lv_obj_add_flag(squint[e][i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static lv_obj_t* rect(lv_obj_t* parent, int x, int y, int w, int h, lv_color_t c) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

// The body's shape for a pose: a flat-topped rectangle, narrower than the
// panel by `margin` each side with an arm nub filling each gap.
static void shape_of(const FacePose* p, FieldShape* s) {
    field_shape_init(s);
    if (p->y >= H + geom.cell * 64) return;           // far below: nothing
    s->body.x1 = p->margin;
    s->body.x2 = W - 1 - p->margin;
    s->body.y1 = p->y;
    s->body.y2 = H - 1;
    if (p->margin <= 0) return;
    const int aw = face_arm_w(&geom), ah = face_arm_h(&geom);
    const int32_t xl = s->body.x1, xr = s->body.x2;
    const int32_t ya = p->y + arm_top + p->arm_dy;
    s->extra[0].x1 = xl - aw; s->extra[0].x2 = xl - 1; s->extra[0].y1 = ya; s->extra[0].y2 = ya + ah - 1;
    s->extra[1].x1 = xr + 1;  s->extra[1].x2 = xr + aw; s->extra[1].y1 = ya; s->extra[1].y2 = ya + ah - 1;
}

static EyeBox eye_next[2];

static void set_eye_objs(void* ctx) {
    (void)ctx;
    for (int e = 0; e < 2; e++) {
        const EyeBox& b = eye_next[e];
        const EyeBox& l = eye_last[e];
        if (b.w != l.w || b.h != l.h) lv_obj_set_size(eye_box[e], b.w, b.h);
        if (b.ew != l.ew || b.eh != l.eh) {
            lv_obj_set_size(eye_open[e], b.ew, b.eh);
            lv_obj_set_pos(eye_open[e], squinting ? S(30) - b.ew / 2 : 0, 0);
        }
        if (b.x != l.x || b.y != l.y) lv_obj_set_pos(eye_box[e], b.x, b.y);
    }
}

static lv_area_t eye_area(const EyeBox& b) {
    lv_area_t a = { b.x, b.y, b.x + b.w - 1, b.y + b.h - 1 };
    return a;
}

// The eyes ride the body, lag it by eye_dy (vertically), stretch or squash
// about their centre, and glance. An open eye's box is exactly the eye, a
// solid rectangle, so it moves with LVGL's invalidation paused and only what
// changed (old XOR new) is redrawn, a few rows or columns instead of the
// whole eye twice. A squint widens the box to hold the chevron and moves the
// ordinary way.
static void apply_eyes(void) {
    int dl, dr;
    face_eye_offsets(glance_cur, &dl, &dr);
    const int dx[2] = { dl, dr };
    const int st = pose.stretch;
    int h = eye_h0 * (1000 + st) / 1000;
    int w = eye_w0 * 1000 / (1000 + st);
    h += h & 1; w += w & 1;
    for (int e = 0; e < 2; e++) {
        const int cx = eye_cx[e] + dx[e];
        const int cy = pose.y + eye_top + eye_h0 / 2 + pose.eye_dy;
        EyeBox& b = eye_next[e];
        if (squinting) { b.x = cx - S(30); b.y = cy - eye_h0 / 2; b.w = box_w0; b.h = box_h0; b.ew = eye_w0; b.eh = eye_h0; b.solid = false; }
        else           { b.x = cx - w / 2; b.y = cy - h / 2;      b.w = w;      b.h = h;      b.ew = w;      b.eh = h;      b.solid = true; }
    }
    bool changed = false, quiet = true;
    for (int e = 0; e < 2; e++) {
        const EyeBox& b = eye_next[e];
        const EyeBox& l = eye_last[e];
        if (b.x != l.x || b.y != l.y || b.w != l.w || b.h != l.h || b.ew != l.ew || b.eh != l.eh)
            changed = true;
        if (!b.solid || !l.solid) quiet = false;
    }
    if (!changed) return;
    if (quiet) {
        field_quietly(root, set_eye_objs, NULL);
        for (int e = 0; e < 2; e++) {
            const lv_area_t o = eye_area(eye_last[e]), n = eye_area(eye_next[e]);
            field_invalidate_change(root, &o, &n);
        }
    } else {
        set_eye_objs(NULL);
    }
    eye_last[0] = eye_next[0];
    eye_last[1] = eye_next[1];
}

static void apply_pose(void) {
    FieldShape s;
    shape_of(&pose, &s);
    field_set_shape(&field, &s);
    apply_eyes();
}

static void place_eyes(int g) {
    glance_cur = g;
    apply_eyes();
}

static FacePose gone_pose(void) {
    FacePose p = {};
    p.y = H + geom.cell * 64;
    return p;
}

void face_init(void) {
    const BoardCaps& c = board_caps();
    W = c.width; H = c.height;
    mind = W < H ? W : H;
    eye_cx[0] = W / 2 - S(80);
    eye_cx[1] = W / 2 + S(80);
    eye_top = (H - mind) / 2 + S(150);
    arm_top = (H - mind) / 2 + S(240);
    eye_w0 = S(44); eye_h0 = S(120);
    glance_px = S(20);

    geom.w = W; geom.h = H; geom.mind = mind;
    geom.cell = mind / 60 > 0 ? mind / 60 : 1;
    geom.start_y = H;

    field_init(&field, lv_layer_top(), W, H, COL_FACE);
    root = field.obj;
    const int u = S(12) > 0 ? S(12) : 1;
    // The box spans the open eye and the squint chevron (which reaches 2u
    // past the eye's right edge); its children are box-local. Kept tight:
    // moving it redraws its whole area every frame of a rise.
    box_w0 = S(52) > 6 * u ? S(52) : 6 * u;
    box_h0 = S(120) > S(20) + 9 * u ? S(120) : S(20) + 9 * u;
    for (int e = 0; e < 2; e++) {
        eye_box[e] = lv_obj_create(root);
        lv_obj_remove_style_all(eye_box[e]);
        lv_obj_set_size(eye_box[e], box_w0, box_h0);
        lv_obj_set_pos(eye_box[e], 0, H + S(200));
        lv_obj_clear_flag(eye_box[e], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(eye_box[e], LV_OBJ_FLAG_SCROLLABLE);
        eye_open[e] = rect(eye_box[e], S(8), 0, eye_w0, eye_h0, COL_EYE);
        const int dir = e == 0 ? +1 : -1;      // left eye ">", right eye "<"
        for (int i = 0; i < 5; i++) {
            const int dx = (dir > 0 ? i : 4 - i) * u;
            squint[e][i] = rect(eye_box[e], dx, S(20) + i * u, 2 * u, u, COL_EYE);
            if (i < 4)
                squint[e][5 + i] = rect(eye_box[e], dx, S(20) + (8 - i) * u, 2 * u, u, COL_EYE);
        }
        eye_last[e] = { INT32_MIN, 0, box_w0, box_h0, -1, -1, false };   // forces the first placement
    }
    pose = gone_pose();
    place_eyes(0);
    set_squint(false);
    clawd_rng_seed(&rng, millis() ^ 0xFACE);
}

// Down and out of sight: hidden, so nothing of it can linger at the bottom
// edge. Its eyes sit below the panel, so hiding repaints nothing.
static void rest_hidden(void) {
    pose = gone_pose();
    apply_pose();
    visible = false;
    field_hide(&field);
}

static PerfWindow perf_window(void) {
    if (move_kind == FACE_MOVE_DROP) return PERF_FACE_DROP;
    return settling ? PERF_FACE_SETTLE : PERF_FACE_RISE;
}

static void motion_exec(void* var, int32_t v) {
    (void)var;
    if (move_kind == FACE_MOVE_RISE && !settling && (uint32_t)v >= face_motion_rest_ms()) {
        perf_end(PERF_FACE_RISE);
        settling = true;
        perf_begin(PERF_FACE_SETTLE);
    }
    pose = face_motion_pose(move_kind, (uint32_t)v, &geom);
    apply_pose();
}

static void motion_done(lv_anim_t* a) {
    (void)a;
    moving = false;
    perf_end(perf_window());
    if (move_kind == FACE_MOVE_DROP) rest_hidden();
}

static void start_motion(FaceMove m) {
    if (!root) return;
    lv_anim_delete(&anim_var, NULL);
    if (moving) perf_end(perf_window());
    if (!visible) {
        // Coming up from all the way down: nothing on screen yet, so it
        // shows and goes on top of the overlay layer without a redraw.
        pose = gone_pose();
        apply_pose();
        field_show(&field);
        field_to_front(&field);
        geom.start_y = H;
    } else {
        geom.start_y = pose.y < H ? pose.y : H;
    }
    move_kind = m;
    settling = false;
    visible = true;
    moving = true;
    perf_begin(perf_window());
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &anim_var);
    lv_anim_set_exec_cb(&a, motion_exec);
    const uint32_t ms = face_motion_duration(m);
    lv_anim_set_values(&a, 0, (int32_t)ms);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);   // the value is elapsed ms
    lv_anim_set_completed_cb(&a, motion_done);
    lv_anim_start(&a);
}

static void rise(int interest) {
    if (!root) return;
    interest_g = interest;
    blink_phase = 0; glance_phase = 0;
    set_squint(false);
    lv_anim_delete(&glance_cur, NULL);
    glance_cur = 0;
    start_motion(FACE_MOVE_RISE);
    next_act_at = millis() + face_motion_duration(FACE_MOVE_RISE) + 1200;
}

void face_rise(void) { rise(0); }

// A peek: curious about where you tapped, so its resting glances keep going
// back there.
void face_rise_toward(int x) {
    const int g = peek_glance_px(x, W, glance_px * 3 / 2);
    rise(g != 0 ? g : 1);
}

void face_drop(void) {
    if (!root || !visible) return;
    resting = false;
    // Eyes open and front-facing as it goes: he's looking at you, then away.
    blink_phase = 0;
    set_squint(false);
    lv_anim_delete(&glance_cur, NULL);
    glance_phase = 0; glance_cur = 0;
    interest_g = 0;
    start_motion(FACE_MOVE_DROP);
}

void face_hide_now(void) {
    if (!root) return;
    lv_anim_delete(&anim_var, NULL);
    lv_anim_delete(&glance_cur, NULL);
    if (moving) perf_end(perf_window());
    moving = false; resting = false;
    blink_phase = 0; glance_phase = 0; glance_cur = 0; interest_g = 0;
    set_squint(false);
    rest_hidden();
}

bool face_visible(void) { return visible; }
bool face_moving(void)  { return moving; }
bool face_busy(void)    { return moving || blink_phase != 0 || glance_phase != 0; }

void face_blink(bool twice) {
    if (!visible) return;
    blink_twice = twice;
    blink_phase = 1;
    blink_until = millis() + BLINK_MS;
    set_squint(true);
}

void face_set_resting(bool on) {
    resting = on;
    if (on) next_act_at = millis() + 800;
}

static void glance_to(int target);

void face_look_at_x(int x, uint32_t hold_ms) {
    if (!visible) return;
    glance_hold = hold_ms ? hold_ms : 1;
    glance_phase = 1;
    glance_target = peek_glance_px(x, W, glance_px * 3 / 2);
    glance_to(glance_target);
}

static void glance_exec(void* var, int32_t v) {
    (void)var;
    place_eyes(v);
}

static void glance_done(lv_anim_t* a) {
    (void)a;
    if (glance_phase == 1) {
        glance_phase = 2;
        glance_until = millis() + (glance_hold ? glance_hold : clawd_rng_range(&rng, 700, 1600));
    } else {
        glance_phase = 0;
    }
}

static void glance_to(int target) {
    lv_anim_delete(&glance_cur, NULL);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &glance_cur);
    lv_anim_set_exec_cb(&a, glance_exec);
    lv_anim_set_values(&a, glance_cur, target);
    lv_anim_set_duration(&a, GLANCE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, glance_done);
    lv_anim_start(&a);
}

void face_tick(void) {
    if (!root || !visible) return;
    const uint32_t now = millis();

    if (blink_phase && (int32_t)(now - blink_until) >= 0) {
        if (blink_phase == 1 && blink_twice) {
            blink_phase = 2; blink_until = now + BLINK_GAP; set_squint(false);
        } else if (blink_phase == 2) {
            blink_phase = 3; blink_until = now + BLINK_MS; set_squint(true);
        } else {
            blink_phase = 0; set_squint(false);
        }
    }
    if (glance_phase == 2 && (int32_t)(now - glance_until) >= 0) {
        glance_phase = 3;
        glance_to(0);
    }

    if (!resting || moving || blink_phase || glance_phase) return;
    if ((int32_t)(now - next_act_at) < 0) return;
    const FacePlan p = face_plan_next(&rng, &clawd_tuning());
    switch (p.act) {
    case FACE_BLINK:         face_blink(false); break;
    case FACE_DOUBLE_BLINK:  face_blink(true); break;
    case FACE_GLANCE_LEFT:
    case FACE_GLANCE_RIGHT:
        glance_hold = 0;
        glance_phase = 1;
        glance_target = face_glance_px(p.act, glance_px, interest_g, &rng, &clawd_tuning());
        glance_to(glance_target);
        break;
    }
    next_act_at = now + p.gap_ms;
}
