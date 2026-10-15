#include "mascot.h"
#include "clawd_anim.h"
#include "clawd_policy.h"
#include "splash_geometry.h"
#include "usage_rate.h"
#include "hal/board_caps.h"
#include <Arduino.h>
#include <string.h>
#include <esp_heap_caps.h>

// ─── Sprites ─────────────────────────────────────────────────────────────────
static lv_obj_t*      img = NULL;        // corner Clawd
static uint8_t*       buf = NULL;        // planar RGB565A8, sized for the largest corner act
static lv_image_dsc_t dsc;
static lv_obj_t*      big_img = NULL;    // lurk + attention Clawd (PSRAM only)
static uint8_t*       big_buf = NULL;
static lv_image_dsc_t big_dsc;

static int cell = 3, slot_x = 0, feet_y = 0, screen_w = 480, screen_h = 480;
static bool round_panel = false;
static int lurk_cell = 8, lurk_feet_y = 384;
static int big_cell = 12, big_x0 = 0, big_feet_y = 420;

// ─── State ───────────────────────────────────────────────────────────────────
enum Mode {
    M_SEATED,     // still pose in the seat
    M_ACT,        // playing an act in the seat
    M_WALK,       // corner walking (off an edge or back to the seat)
    M_AWAY,       // off screen
    M_LURK,       // full-size lurk peeking in from the right edge
    M_BIG_IN,     // attention Clawd walking on
    M_BIG_ACT,    // ... waving / pointing
    M_BIG_OUT,    // ... walking off
    M_HELD,       // minimal mode attention: a held pointing frame
    M_SEAT_ATTN,  // attention without the full-size sprite: waving / pointing in his seat
};
enum After { AFTER_SEAT, AFTER_AWAY, AFTER_LURK, AFTER_BIG };

static Mode     mode = M_SEATED;
static const ClawdAnim* anim = NULL;
static const ClawdAnim* walk_anim = NULL;
static uint16_t frame = 0;
static uint32_t frame_started = 0;
static bool     from_loop = false;
static int      x = 0;                 // corner widget x, px (may be off screen)
static int      facing = +1;
static uint16_t speed = 100;
static int      walk_target = 0;
static After    after = AFTER_SEAT;
static uint8_t  act_left = 0;
static bool     act_auto = false;      // current act was his own idea
static bool     autonomy = false;
static bool     minimal = false;
static bool     attn = false;
static bool     stay_away = false;     // asked to leave mid-lurk: don't walk back in
static bool     hurry = false;         // cut the lurk short: walk back at HURRY_PCT
static bool     attn_to_seat = true;   // attention over: the small one walks home (else stays out)
static uint32_t seated_since = 0;
static uint32_t idle_gap = 7000;
static const char* last_act = NULL;
static int      big_dx = 0;
static uint8_t  big_act = 0;           // 0 waving, 1 pointing (attention, either size)
static ClawdRng rng;

// Per mood group: how long he sits between acts and which acts he picks.
// "lurking" = the walk-off / full-size lurk / walk-back trip.
static const char* const ACTS_BY_RATE[4][4] = {
    { "pointing", "lurking", NULL,       NULL      },   // idle: sparse, sneaky
    { "waving",   "lurking", "pointing", NULL      },   // normal
    { "waving",   "dancing", "lurking",  NULL      },   // active
    { "dancing",  "waving",  "dancing",  "lurking" },   // heavy: can't sit still
};
static const uint16_t STILL_MS_BY_RATE[4] = { 10000, 7000, 5000, 3500 };
static const uint16_t HURRY_PCT = 250;        // hurrying home from a lurk trip
// An act taller than the headroom above his seat (jumping happy on a 240 px
// panel) would be clipped by the top edge. It swaps for this official act,
// which always fits, instead.
static const char* const TALL_FALLBACK = "dancing";
// Everything the corner buffer must fit.
static const char* const CORNER_ANIMS[] = {
    "walking", "waving", "pointing", "dancing", "jumping happy",
};

// ─── Rendering ───────────────────────────────────────────────────────────────
// One frame into a planar RGB565A8 image (alpha 0 outside the art), widget
// anchored on the feet line.
static void render(const ClawdAnim* a, uint16_t f, bool mirror, lv_image_dsc_t* d,
                   uint8_t* b, lv_obj_t* o, int c, int px, int feet) {
    if (!a || !b || !o) return;
    const int w = a->w * c, h = a->h * c;
    uint16_t* color = (uint16_t*)b;
    uint8_t*  alpha = b + (size_t)w * h * 2;
    const uint8_t* src = clawd_frame(a, f);
    for (int gy = 0; gy < a->h; gy++) {
        for (int gx = 0; gx < a->w; gx++) {
            uint8_t code = src[gy * a->w + (mirror ? a->w - 1 - gx : gx)];
            uint16_t col = (code && code < CLAWD_PALETTE_SIZE) ? a->palette[code] : 0;
            uint8_t  al  = code ? 255 : 0;
            for (int dy = 0; dy < c; dy++) {
                uint16_t* cp = &color[(gy * c + dy) * w + gx * c];
                uint8_t*  ap = &alpha[(gy * c + dy) * w + gx * c];
                for (int dx = 0; dx < c; dx++) { cp[dx] = col; ap[dx] = al; }
            }
        }
    }
    d->header.w = w;
    d->header.h = h;
    d->header.cf = LV_COLOR_FORMAT_RGB565A8;
    d->header.stride = w * 2;
    d->data = b;
    d->data_size = (size_t)w * h * 3;
    lv_image_set_src(o, d);
    lv_obj_set_pos(o, px, feet - h);
    lv_obj_invalidate(o);
}

static void show(lv_obj_t* o, bool on) {
    if (!o) return;
    if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else               lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// Where the glass ends on a row: the panel's edge, or the circle's on a round
// panel, so nobody walks on or off through the part that isn't there.
static int  left_edge(int y)  { int a, b; panel_row_span(screen_w, screen_h, round_panel, y, &a, &b); return a; }
static int  right_edge(int y) { int a, b; panel_row_span(screen_w, screen_h, round_panel, y, &a, &b); return b; }

static void draw_corner(void) {
    render(anim, frame, facing < 0, &dsc, buf, img, cell, x, feet_y);
}
static void draw_big(void) {
    render(anim, frame, mode == M_BIG_OUT, &big_dsc, big_buf, big_img,
           big_cell, big_x0 + anim->ox * big_cell + big_dx, big_feet_y);
}
static void draw_lurk(void) {
    // He left stage left, so he peeks in from the RIGHT edge — mirrored (the
    // art is authored hanging off the left edge).
    render(anim, frame, true, &big_dsc, big_buf, big_img, lurk_cell,
           right_edge(lurk_feet_y) - anim->w * lurk_cell, lurk_feet_y);
}

static int  off_left_x(void) {
    return offstage_left_x(screen_w, screen_h, round_panel, feet_y, walk_anim ? walk_anim->w * cell : 100);
}
static int  off_right_x(void) { return offstage_right_x(screen_w, screen_h, round_panel, feet_y); }

static void seat(void) {
    mode = M_SEATED;
    anim = walk_anim;               // frame 0 == the official still pose
    frame = 0;
    x = slot_x;
    facing = +1;
    act_left = 0;
    seated_since = millis();
    int g = usage_rate_group();
    if (g < 0 || g > 3) g = 0;
    // Liveliness scales the still time; never under 1.5 s, so even
    // All-nonsense lets him land between acts.
    uint32_t still = (uint32_t)STILL_MS_BY_RATE[g] * clawd_tuning().idle_gap_pct / 100;
    if (still < 1500) still = 1500;
    idle_gap = clawd_jitter(still, &rng);
    show(img, true);
    draw_corner();
}

static void start_frames(const ClawdAnim* a) {
    anim = a;
    frame = 0;
    from_loop = false;
    frame_started = millis();
}

static void walk_to(int target, uint16_t spd, After then) {
    start_frames(walk_anim);
    speed = spd;
    walk_target = target;
    after = then;
    facing = target < x ? -1 : +1;
    mode = M_WALK;
    show(img, true);
    draw_corner();
}

static void big_enter(void) {
    if (!big_img || !walk_anim) { mode = M_AWAY; return; }
    show(img, false);
    mode = M_BIG_IN;
    start_frames(walk_anim);
    frame = walk_anim->loop_start;   // mid-stride: the earlier frames play off screen
    speed = 200;
    big_dx = left_edge(big_feet_y) - (big_x0 + (walk_anim->ox + walk_anim->w) * big_cell);
    show(big_img, true);
    lv_obj_move_foreground(big_img);
    draw_big();
}

static void seat_attention(void) {
    big_act = 0;
    start_frames(clawd_anim("waving"));
    speed = 100;
    facing = +1;
    x = slot_x;
    mode = M_SEAT_ATTN;
    draw_corner();
}

static void lurk_start(void) {
    const ClawdAnim* l = clawd_anim("lurking");
    if (!l || !big_img) { walk_to(slot_x, 100, AFTER_SEAT); return; }
    mode = M_LURK;
    start_frames(l);
    speed = 100;
    show(big_img, true);
    lv_obj_move_foreground(big_img);
    draw_lurk();
}

// ─── API ─────────────────────────────────────────────────────────────────────
lv_obj_t* mascot_create(lv_obj_t* parent, int sx, int feet, int c) {
    cell = c; slot_x = sx; feet_y = feet;
    const BoardCaps& caps = board_caps();
    screen_w = caps.width;
    screen_h = caps.height;
    round_panel = caps.round_panel;
    walk_anim = clawd_anim("walking");
    if (!walk_anim) return NULL;
    clawd_rng_seed(&rng, millis() ^ 0xC1A3D);

    int mw = 0, mh = 0;
    for (const char* n : CORNER_ANIMS) {
        const ClawdAnim* a = clawd_anim(n);
        if (!a) continue;
        if (a->w > mw) mw = a->w;
        if (a->h > mh) mh = a->h;
    }
    const size_t bytes = (size_t)(mw * c) * (mh * c) * 3;

    const int mind = caps.width < caps.height ? caps.width : caps.height;
    lurk_cell = mind / SPLASH_GRID;
    if (lurk_cell < 1) lurk_cell = 1;
    big_cell = lurk_cell * 3 / 2;
    if (big_cell < 1) big_cell = 1;
    big_feet_y = caps.height * 7 / 8;
    big_x0 = (caps.width - walk_anim->w * big_cell) / 2 - walk_anim->ox * big_cell;
    const ClawdAnim* lurk = clawd_anim("lurking");
    if (lurk) lurk_feet_y = ((SPLASH_GRID - CLAWD_ART_H) / 2 + lurk->oy + lurk->h) * lurk_cell;
    size_t big_bytes = 0;
    static const char* const BIG[] = { "lurking", "walking", "waving", "pointing" };
    for (const char* n : BIG) {
        const ClawdAnim* a = clawd_anim(n);
        if (!a) continue;
        const int cl = (a == lurk) ? lurk_cell : big_cell;
        const size_t b = (size_t)(a->w * cl) * (a->h * cl) * 3;
        if (b > big_bytes) big_bytes = b;
    }

    buf = (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!buf) buf = (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) return NULL;
    big_buf = big_bytes ? (uint8_t*)heap_caps_malloc(big_bytes, MALLOC_CAP_SPIRAM) : NULL;

    img = lv_image_create(parent);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    if (big_buf) {
        big_img = lv_image_create(parent);
        lv_obj_clear_flag(big_img, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(big_img, LV_OBJ_FLAG_HIDDEN);
    }
    seat();
    return img;
}

bool mascot_seated(void)  { return img && mode == M_SEATED; }
// Without a corner sprite (allocation failed, static logo instead) he counts
// as away, so the director's beats never wait on him.
bool mascot_away(void)    { return !img || mode == M_AWAY; }
bool mascot_busy(void)    { return img && mode != M_SEATED && mode != M_AWAY && mode != M_HELD; }
bool mascot_acting(void)  { return img && mode == M_ACT; }
bool mascot_free(void) {
    return img && !minimal && (mode == M_SEATED || (mode == M_ACT && act_auto));
}
bool mascot_out(void) {
    return img && !minimal &&
           (mode == M_LURK || mode == M_AWAY || (mode == M_WALK && after != AFTER_BIG));
}
bool mascot_in_attention(void) {
    return attn || mode == M_BIG_IN || mode == M_BIG_ACT || mode == M_BIG_OUT ||
           mode == M_HELD || mode == M_SEAT_ATTN || (mode == M_WALK && after == AFTER_BIG);
}

void mascot_autonomy(bool on) {
    autonomy = on;
    if (on && mode == M_SEATED) seated_since = millis();
}

bool mascot_act(const char* name, uint8_t times, bool snappy, uint16_t speed_pct) {
    if (!img || minimal) return false;
    if (!(mode == M_SEATED || (mode == M_ACT && act_auto))) return false;
    const ClawdAnim* a = clawd_anim(name);
    if (!a) return false;
    if (feet_y - a->h * cell < 0) {
        const ClawdAnim* f = clawd_anim(TALL_FALLBACK);
        if (f && feet_y - f->h * cell >= 0) a = f;
    }
    start_frames(a);
    if (snappy && a->frame_count > 1) frame = 1;
    speed = speed_pct ? speed_pct : 100;   // not whatever the last walk ran at
    act_left = times ? times : 1;
    act_auto = false;
    facing = +1;
    mode = M_ACT;
    draw_corner();
    return true;
}

void mascot_leave(uint16_t spd) {
    if (!img || minimal) return;
    if (mode == M_AWAY) return;
    if (mode == M_LURK) {            // finish the peek (no slower than asked), then stay out
        stay_away = true;
        if (spd > speed) speed = spd;
        return;
    }
    if (mode == M_WALK && after != AFTER_SEAT) {
        speed = spd;
        if (after == AFTER_LURK) after = AFTER_AWAY;   // heading out for a lurk: just out
        return;
    }
    // Out through the nearest edge (he may be on his way back from the right).
    const int target = x > screen_w / 2 ? off_right_x() : off_left_x();
    walk_to(target, spd, AFTER_AWAY);
}

void mascot_return(int from_side, uint16_t spd) {
    if (!img) return;
    if (mode == M_WALK && after != AFTER_SEAT) {   // still leaving: turn around
        walk_to(slot_x, spd, AFTER_SEAT);
        return;
    }
    if (mode == M_LURK) { stay_away = false; return; }   // he walks back in after the peek anyway
    if (mode != M_AWAY) return;
    x = from_side > 0 ? off_right_x() : off_left_x();
    walk_to(slot_x, spd, AFTER_SEAT);
}

bool mascot_lurk(void) {
    if (!img || minimal || !big_img || mode != M_SEATED) return false;
    last_act = "lurking";
    walk_to(off_left_x(), 100, AFTER_LURK);
    return true;
}

bool mascot_hurry_home(void) {
    if (!img || minimal) return false;
    switch (mode) {
    case M_SEATED:
    case M_ACT:
        return true;                                  // already home
    case M_LURK:
        // Cut the peek short: the rest of it plays fast, then he walks back
        // in at a hurry (lurk end, mascot_tick).
        stay_away = false;
        hurry = true;
        speed = HURRY_PCT + 50;
        return true;
    case M_WALK:
        if (after == AFTER_BIG) return false;         // fetching you: attention owns him
        if (after == AFTER_SEAT) { speed = HURRY_PCT; return true; }
        walk_to(slot_x, HURRY_PCT, AFTER_SEAT);       // heading out: turn around
        return true;
    case M_AWAY:
        x = off_left_x();
        walk_to(slot_x, HURRY_PCT, AFTER_SEAT);
        return true;
    default:
        return false;                                 // the big Clawd is on
    }
}

void mascot_place_away(void) {
    if (!img) return;
    stay_away = false;
    hurry = false;
    show(img, false);
    show(big_img, false);
    mode = M_AWAY;
    x = off_left_x();
}

void mascot_reset(void) {
    if (!img) return;
    attn = false;
    stay_away = false;
    hurry = false;
    show(big_img, false);
    seat();
}

void mascot_set_minimal(bool on) {
    minimal = on;
    mascot_reset();
}

void mascot_attention(bool on, bool to_seat) {
    if (!img || on == attn) return;
    attn = on;
    attn_to_seat = on || to_seat;
    if (minimal) {
        if (on) {
            const ClawdAnim* p = clawd_anim("pointing");
            if (!p) return;
            mode = M_HELD;
            anim = p;
            frame = p->frame_count > 16 ? 16 : p->frame_count - 1;   // the point
            x = slot_x; facing = +1;
            draw_corner();
        } else {
            seat();
        }
        return;
    }
    if (!big_img) {                                // no PSRAM: he waves and points from his seat
        if (on) {
            // He may be out (attention cut a show short): he walks back to
            // his seat first, and mascot_tick starts the wave there.
            if (mode == M_SEATED || mode == M_ACT) seat_attention();
            else mascot_return(-1, 150);
        } else if (mode == M_SEAT_ATTN) {
            seat();
        }
        return;
    }
    if (on) {
        if (mode == M_AWAY)              big_enter();
        else if (mode == M_LURK)       { show(big_img, false); big_enter(); }
        else if (mode == M_BIG_OUT)      mode = M_BIG_IN;          // was leaving: turn around
        else                             walk_to(off_left_x(), 200, AFTER_BIG);
    } else {
        if (mode == M_WALK && after == AFTER_BIG) {               // hadn't left yet
            if (attn_to_seat) walk_to(slot_x, 200, AFTER_SEAT);
            else              after = AFTER_AWAY;                 // keep going: he's off again
        }
        else if (mode == M_BIG_IN || mode == M_BIG_ACT) {
            mode = M_BIG_OUT;
            start_frames(walk_anim);
            speed = 200;
            draw_big();
        }
    }
}

// ─── Tick ────────────────────────────────────────────────────────────────────
static void seated_tick(uint32_t now) {
    if (!autonomy || minimal || now - seated_since < idle_gap) return;
    int g = usage_rate_group();
    if (g < 0 || g > 3) g = 0;
    const int pick = clawd_pick_no_repeat(ACTS_BY_RATE[g], 4, last_act, &rng);
    if (pick < 0) { seated_since = now; return; }
    const char* act = ACTS_BY_RATE[g][pick];
    last_act = act;
    if (strcmp(act, "lurking") == 0) {
        if (!big_img) { seated_since = now; return; }  // no full-size sprite: skip the trip
        walk_to(off_left_x(), 100, AFTER_LURK);
        return;
    }
    const ClawdAnim* a = clawd_anim(act);
    if (!a) { seated_since = now; return; }
    start_frames(a);
    speed = 100;
    act_left = 1;
    act_auto = true;
    mode = M_ACT;
    draw_corner();
}

void mascot_tick(void) {
    if (!img || !anim) return;
    const uint32_t now = millis();
    if (mode == M_SEATED) {
        if (attn && !big_img && !minimal) { seat_attention(); return; }   // back in his seat for the alert
        seated_tick(now);
        return;
    }
    if (mode == M_AWAY || mode == M_HELD) return;

    const ClawdAnim* a = anim;
    if (now - frame_started < clawd_hold(a, frame, speed)) return;
    frame_started = now;

    const bool walking = mode == M_WALK || mode == M_BIG_IN || mode == M_BIG_OUT;
    uint16_t next = frame + 1;
    if (walking && frame == a->loop_end) next = a->loop_start;   // hold the gait loop

    if (next >= a->frame_count) {
        if (mode == M_BIG_ACT || mode == M_SEAT_ATTN) {   // wave, point, wave, ...
            big_act ^= 1;
            start_frames(clawd_anim(big_act ? "pointing" : "waving"));
            speed = 100;
            if (mode == M_BIG_ACT) draw_big();
            else                   draw_corner();
            return;
        }
        if (mode == M_LURK) {                          // back in from the right
            show(big_img, false);
            if (stay_away) { stay_away = false; hurry = false; mode = M_AWAY; return; }
            x = off_right_x();
            walk_to(slot_x, hurry ? HURRY_PCT : 100, AFTER_SEAT);
            hurry = false;
            return;
        }
        if (mode == M_ACT && --act_left > 0) {         // again (jumping happy x2)
            frame = 0;
            draw_corner();
            return;
        }
        seat();                                        // acts end on the idle pose
        return;
    }

    const bool was_loop = from_loop;
    frame = next;
    from_loop = walking && frame >= a->loop_start && frame <= a->loop_end;

    if (mode == M_BIG_IN || mode == M_BIG_OUT) {
        if (from_loop) {
            const int step = clawd_gait_cells(GAIT_WALK, frame, was_loop) * big_cell;
            big_dx += (mode == M_BIG_IN) ? step : -step;
            if (mode == M_BIG_IN && big_dx >= 0) {     // on his spot: wave
                big_dx = 0;
                big_act = 0;
                mode = M_BIG_ACT;
                start_frames(clawd_anim("waving"));
                speed = 100;
                draw_big();
                return;
            }
            if (mode == M_BIG_OUT && big_x0 + (a->ox + a->w) * big_cell + big_dx <= left_edge(big_feet_y)) {
                show(big_img, false);                  // gone: the small one walks home
                x = off_left_x();
                if (!attn_to_seat) { mode = M_AWAY; return; }   // ...or stays out (screensaver resumes)
                walk_to(slot_x, 100, AFTER_SEAT);
                return;
            }
        }
        draw_big();
        return;
    }
    if (mode == M_LURK) { draw_lurk(); return; }
    if (mode == M_BIG_ACT) { draw_big(); return; }

    if (mode == M_WALK && from_loop) {
        const int dir = walk_target < x ? -1 : +1;
        x += dir * clawd_gait_cells(GAIT_WALK, frame, was_loop) * cell;
        if ((dir > 0 && x >= walk_target) || (dir < 0 && x <= walk_target)) {
            x = walk_target;
            if (after == AFTER_SEAT) { seat(); return; }
            show(img, false);
            mode = M_AWAY;
            if (after == AFTER_LURK) lurk_start();
            else if (after == AFTER_BIG) big_enter();
            return;
        }
    }
    draw_corner();
}
