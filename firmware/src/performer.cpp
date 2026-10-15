#include "performer.h"
#include "clawd_anim.h"
#include "clawd_policy.h"
#include "field.h"
#include "perf.h"
#include "splash_geometry.h"
#include "hal/board_caps.h"
#include <Arduino.h>
#include <string.h>
#include <esp_heap_caps.h>

// ─── Geometry ────────────────────────────────────────────────────────────────
// 60x60-cell grid; cell = min(W,H)/60 (8 px on 480, 6 on 368, 4 on 240). The
// official 55x37 art stage sits at a fixed anchor on the grid and every
// animation is placed at its authored offset, so the shared idle pose never
// jumps between animations. The grid is shifted vertically so his feet stand
// on the floor the meters give him.
#define GRID      SPLASH_GRID
#define ANCHOR_X  ((GRID - CLAWD_ART_W) / 2)
#define ANCHOR_Y  ((GRID - CLAWD_ART_H) / 2)

static int       cell = 8;
static int       dim = GRID * 8;               // the buffer is dim x dim px
static lv_obj_t* img = NULL;
static uint8_t*  buf = NULL;                   // planar RGB565A8: colour plane, then alpha
static lv_image_dsc_t dsc;
static bool      shown = false;                // img not hidden

// ─── Mood groups ─────────────────────────────────────────────────────────────
#define GROUP_COUNT 4
#define GROUP_MAX   4
static const char* const GROUP_NAMES[GROUP_COUNT][GROUP_MAX] = {
    { "magnifier", "walking", "pointing", "lurking" },        // idle / sleepy
    { "crab walking", "waving", "trumpet", "basketball" },    // normal pace
    { "laptop", "dancing", "skateboard", "soccer" },          // active
    { "racing car", "cloud", "sailing scene", "jumping happy" }, // heavy burn
};
// The screensaver's calm dips: gentle Idle-group scenes only (lurking is a
// corner act and pointing is his "noticing" gesture).
static const char* const CALM_NAMES[] = { "magnifier", "walking" };

// ─── Performer ───────────────────────────────────────────────────────────────
enum PerfState { P_OFF, P_STAND, P_WALK, P_SCENE, P_CHOREO };
static PerfState pst = P_OFF;
static const ClawdAnim* stand_anim = NULL;   // "walking" frame 0 = official still
static const ClawdAnim* cur = NULL;          // animation being rendered
static uint16_t frame = 0;
static uint32_t frame_started = 0;
static int      dx = 0;                      // cells from the authored home spot
static int      facing = +1;                 // -1 = mirrored (heading left)
static uint16_t speed = 100;
// A walk can ramp its speed up (a run-off that gets going): from ramp_from to
// ramp_to percent over ramp_ms, ease-in. ramp_ms 0 = constant speed.
static uint16_t ramp_from = 100, ramp_to = 100;
static uint32_t ramp_ms = 0, walk_t0 = 0;

// walking
static ClawdGait gait = GAIT_NONE;
static int      walk_target = 0;
static int      walk_dir = 0;
static bool     in_loop = false;
static bool     loop_release = false;
// scenes
static uint32_t loop_hold_ms = 6000;
static uint32_t loop_entered = 0;
static const char* last_show = NULL;         // never the same act twice running,
static const char* last_calm = NULL;         //   tracked per kind of scene
// walker scenes: stand → right edge → off left → home
static uint8_t  choreo_phase = 0;
static uint32_t choreo_since = 0;
static bool     leg_active = false;

static uint8_t         stage_cells[GRID * GRID];
static uint8_t         drawn_cells[GRID * GRID];   // what buf shows
static const uint16_t* drawn_palette = NULL;
static bool            cells_any = false;          // the last composed frame shows any art

static int home_ax(const ClawdAnim* a) { return ANCHOR_X + a->ox; }

// The grid cells that are on the glass on his floor row: all of them on a
// rectangular panel, the circle's chord on a round one. He walks on and off
// at these edges.
static int stage_c0 = 0, stage_c1 = GRID;

static const uint8_t* compose(const ClawdAnim* a, uint16_t f, int ax, bool mirror) {
    memset(stage_cells, 0, sizeof(stage_cells));
    if (!a) return stage_cells;
    const int ay = ANCHOR_Y + a->oy;
    const uint8_t* src = clawd_frame(a, f);
    for (int r = 0; r < a->h; r++) {
        const int dy = ay + r;
        if (dy < 0 || dy >= GRID) continue;
        int c0 = 0, c1 = a->w;
        if (ax + c0 < 0)    c0 = -ax;
        if (ax + c1 > GRID) c1 = GRID - ax;
        if (c0 >= c1) continue;
        if (mirror) {
            for (int c = c0; c < c1; c++)
                stage_cells[dy * GRID + ax + c] = src[r * a->w + (a->w - 1 - c)];
        } else {
            memcpy(&stage_cells[dy * GRID + ax + c0], &src[r * a->w + c0], c1 - c0);
        }
    }
    return stage_cells;
}

// ─── Rendering ───────────────────────────────────────────────────────────────
// Repaint only the cells that changed and invalidate just their bounding
// box, so a frame costs what moved, not the whole screen. Off stage the
// image is hidden, so the meters' own redraws (the status line, the face
// passing over) never blend a screen-sized transparent image.
static void set_hidden(void* on) {
    if (on) lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_clear_flag(img, LV_OBJ_FLAG_HIDDEN);
}

static void render_cells(const uint8_t* cells, const uint16_t* palette) {
    if (!buf || !img) return;
    bool any = false;
    for (int i = 0; i < GRID * GRID; i++) if (cells[i]) { any = true; break; }

    const bool repaint_all = palette != drawn_palette;
    int gx0 = GRID, gy0 = GRID, gx1 = -1, gy1 = -1;
    uint16_t* color = (uint16_t*)buf;
    uint8_t*  alpha = buf + (size_t)dim * dim * 2;
    for (int gy = 0; gy < GRID; gy++) {
        for (int gx = 0; gx < GRID; gx++) {
            const uint8_t code = cells[gy * GRID + gx];
            const uint8_t was  = drawn_cells[gy * GRID + gx];
            if (code == was && !(repaint_all && code)) continue;
            if (gx < gx0) gx0 = gx;
            if (gx > gx1) gx1 = gx;
            if (gy < gy0) gy0 = gy;
            if (gy > gy1) gy1 = gy;
            const uint16_t c = (code && palette && code < CLAWD_PALETTE_SIZE) ? palette[code] : 0;
            const uint8_t  a = code ? 255 : 0;
            for (int d = 0; d < cell; d++) {
                const size_t row = (size_t)(gy * cell + d) * dim + gx * cell;
                for (int i = 0; i < cell; i++) color[row + i] = c;
                memset(&alpha[row], a, cell);
            }
        }
    }
    memcpy(drawn_cells, cells, sizeof(drawn_cells));
    drawn_palette = palette;
    cells_any = any;

    if (any && !shown) {                          // he's coming on: nothing drawn yet
        field_quietly(img, set_hidden, (void*)0);
        shown = true;
        perf_begin(PERF_PERFORM);
    }
    if (gx1 >= 0) {
        lv_area_t a;
        lv_obj_get_coords(img, &a);
        a.x2 = a.x1 + (gx1 + 1) * cell - 1;
        a.y2 = a.y1 + (gy1 + 1) * cell - 1;
        a.x1 += gx0 * cell;
        a.y1 += gy0 * cell;
        lv_obj_invalidate_area(img, &a);
    }
    if (!any && shown) {                          // gone: his last cells are already invalidated
        field_quietly(img, set_hidden, (void*)1);
        shown = false;
        perf_end(PERF_PERFORM);
    }
}

// Draw whatever the performer currently is.
static void redraw(void) {
    if (pst == P_OFF || !cur) { render_cells(compose(NULL, 0, 0, false), NULL); return; }
    int ax = home_ax(cur) + dx;
    if (pst == P_SCENE && dx == 0) {
        // Edge snap: art touching its canvas's left/right edge was designed to
        // hang off that edge, so it goes to the true screen edge.
        if (cur->ox == 0)                     ax = 0;
        if (cur->ox + cur->w == CLAWD_ART_W)  ax = GRID - cur->w;
    }
    render_cells(compose(cur, frame, ax, facing < 0), cur->palette);
}

static void stand(void) {
    pst = P_STAND;
    cur = stand_anim;
    frame = 0;
    gait = GAIT_NONE;
    redraw();
}

// Off stage: the whole walking frame past that edge.
static int offstage_dx(int side) {
    if (!stand_anim) return side * GRID;
    return side > 0 ? stage_c1 - home_ax(stand_anim) : stage_c0 - (home_ax(stand_anim) + stand_anim->w);
}
static bool is_offstage(void) { return dx >= offstage_dx(+1) || dx <= offstage_dx(-1); }

static void walk_start(const ClawdAnim* a, ClawdGait g, int target, uint16_t spd) {
    cur = a;
    gait = g;
    speed = spd;
    ramp_ms = 0;
    walk_t0 = millis();
    walk_target = target;
    walk_dir = target > dx ? +1 : -1;
    facing = walk_dir;
    frame = 0;
    frame_started = millis();
    in_loop = false;
    loop_release = false;
    redraw();
}

// One step of a walk; returns true when it has finished.
static bool walk_step(uint32_t now) {
    if (ramp_ms) {
        const uint32_t el = now - walk_t0;
        if (el >= ramp_ms) { speed = ramp_to; ramp_ms = 0; }
        else {
            const uint32_t e = el * 1000 / ramp_ms;            // permille
            speed = ramp_from + (uint16_t)((ramp_to - ramp_from) * e * e / 1000000);
        }
    }
    if (now - frame_started < clawd_hold(cur, frame, speed)) return false;
    frame_started = now;
    const bool from_loop = in_loop;
    uint16_t next = frame + 1;
    if (frame == cur->loop_end && !loop_release) next = cur->loop_start;
    if (next >= cur->frame_count) return true;
    frame = next;
    in_loop = frame >= cur->loop_start && frame <= cur->loop_end;
    if (walk_dir != 0 && in_loop) {
        dx += walk_dir * clawd_gait_cells(gait, frame, from_loop);
        if ((walk_dir > 0 && dx >= walk_target) || (walk_dir < 0 && dx <= walk_target)) {
            dx = walk_target;
            walk_dir = 0;
            loop_release = true;
        }
    }
    redraw();
    return false;
}

static ClawdGait gait_of(const ClawdAnim* a) {
    if (!a) return GAIT_NONE;
    if (strcmp(a->name, "walking") == 0)      return GAIT_WALK;
    if (strcmp(a->name, "crab walking") == 0) return GAIT_CRAB;
    return GAIT_NONE;
}

static void scene_start(const ClawdAnim* a, uint32_t hold) {
    if (!a) { stand(); return; }
    Serial.printf("[%lu] performer: scene %s\n", (unsigned long)millis(), a->name);
    dx = 0;
    facing = +1;
    speed = 100;
    loop_hold_ms = hold;
    if (gait_of(a) != GAIT_NONE) {           // walkers do a round trip instead
        pst = P_CHOREO;
        cur = a;
        gait = gait_of(a);
        frame = 0;
        choreo_phase = 0;
        choreo_since = millis();
        leg_active = false;
        walk_dir = 0;
        redraw();
        return;
    }
    pst = P_SCENE;
    cur = a;
    frame = 0;
    frame_started = millis();
    in_loop = false;
    loop_release = false;
    redraw();
}

// ─── Init ────────────────────────────────────────────────────────────────────
void performer_init(lv_obj_t* parent, int feet_y) {
    const BoardCaps& c = board_caps();
    stand_anim = clawd_anim("walking");
    if (!stand_anim) return;
    const SplashGeometry geo = splash_compute_geometry(c.width, c.height, true);
    cell = geo.cell;
    dim = geo.canvas_dim;
    const size_t bytes = (size_t)dim * dim * 3;
    // PSRAM only: without it there's no room for a screen-sized sprite, and
    // the director plays performances as corner acts.
    buf = (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!buf) { Serial.println("performer: no PSRAM, corner acts only"); return; }
    memset(buf, 0, bytes);
    memset(drawn_cells, 0, sizeof(drawn_cells));

    dsc.header.w = dim;
    dsc.header.h = dim;
    dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc.header.stride = dim * 2;
    dsc.data = buf;
    dsc.data_size = bytes;

    img = lv_image_create(parent);
    lv_image_set_src(img, &dsc);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    // His feet (art row 37) on the floor; centred across. The grid may hang
    // past the bottom edge; nothing is drawn there.
    lv_obj_set_pos(img, (c.width - dim) / 2, feet_y - (ANCHOR_Y + CLAWD_ART_H) * cell);
    int x0, x1;
    panel_row_span(c.width, c.height, c.round_panel, feet_y, &x0, &x1);
    span_to_cells((c.width - dim) / 2, cell, GRID, x0, x1, &stage_c0, &stage_c1);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    shown = false;
    pst = P_OFF;
    dx = offstage_dx(-1);
}

bool performer_available(void) { return img != NULL; }

// ─── Performer API ───────────────────────────────────────────────────────────
void performer_place_offstage(int side) {
    dx = offstage_dx(side);
    facing = -side;
    pst = P_OFF;
    cur = NULL;
    redraw();
}

void performer_walk_home(uint16_t spd) {
    if (!stand_anim || !img) return;
    if (dx == 0) { stand(); return; }
    pst = P_WALK;
    walk_start(stand_anim, GAIT_WALK, 0, spd);
}

void performer_walk_off(int side, uint16_t spd) {
    if (!stand_anim || !img) return;
    pst = P_WALK;
    walk_start(stand_anim, GAIT_WALK, offstage_dx(side), spd);
}

void performer_run_off(int side, uint16_t from_pct, uint16_t to_pct, uint32_t ramp) {
    if (!stand_anim || !img) return;
    pst = P_WALK;
    walk_start(stand_anim, GAIT_WALK, offstage_dx(side), from_pct);
    ramp_from = from_pct;
    ramp_to = to_pct;
    ramp_ms = ramp;
}

void performer_perform(int idx, uint32_t loop_ms) {
    if (!img) return;
    scene_start(clawd_anim_at(idx), loop_ms);
}

static ClawdRng rng;
static bool rng_seeded = false;

void performer_perform_group(int group, uint32_t loop_ms, bool calm) {
    if (!img) return;
    if (!rng_seeded) { clawd_rng_seed(&rng, millis() ^ 0x57A6E); rng_seeded = true; }
    if (group < 0 || group >= GROUP_COUNT) group = 0;
    const char* const* names = calm ? CALM_NAMES : GROUP_NAMES[group];
    const int n = calm ? (int)(sizeof(CALM_NAMES) / sizeof(CALM_NAMES[0])) : GROUP_MAX;
    const char*& last = calm ? last_calm : last_show;
    const int pick = clawd_pick_no_repeat(names, n, last, &rng);
    const ClawdAnim* a = pick >= 0 ? clawd_anim(names[pick]) : NULL;
    if (a) last = a->name;
    scene_start(a, loop_ms);
}

void performer_release(void) {
    if (pst == P_OFF) return;
    // Snap to the idle pose where he stands (scenes are authored at home).
    if (pst == P_SCENE) dx = 0;
    facing = +1;
    stand();
}

bool performer_visible(void) { return cells_any; }

bool performer_busy(void) { return pst == P_WALK || pst == P_SCENE || pst == P_CHOREO; }

// ─── Tick ────────────────────────────────────────────────────────────────────
// Where a walker stops at the right edge of the glass, and where he is off it
// to the left; the right edge is never left of where he starts.
static int right_edge_dx(const ClawdAnim* a) {
    const int dx_r = (stage_c1 - a->w) - home_ax(a);
    return dx_r > 0 ? dx_r : 0;
}
static int left_edge_dx(const ClawdAnim* a) {
    const int dx_l = stage_c0 - a->w - home_ax(a);
    return dx_l < -1 ? dx_l : -1;
}

static void choreo_leg(int target) {
    walk_start(cur, gait, target, 100);
    leg_active = true;
}

static void choreo_tick(uint32_t now) {
    const ClawdAnim* a = cur;
    if (leg_active) {
        if (walk_step(now)) {           // leg finished: stand in this gait's idle
            leg_active = false;
            frame = 0;
            choreo_phase++;
            choreo_since = now;
            redraw();
        }
        return;
    }
    switch (choreo_phase) {
    case 0:  // standing at home
        if (now - choreo_since > 1200) { choreo_phase = 1; choreo_leg(right_edge_dx(a)); }
        break;
    case 2:  // at the right edge
        if (now - choreo_since > 1200) { choreo_phase = 3; choreo_leg(left_edge_dx(a)); }
        break;
    case 4:  // off left: a beat, then back home
        if (now - choreo_since > 800) { choreo_phase = 5; choreo_leg(0); }
        break;
    default: // 6: home again
        facing = +1;
        stand();
        break;
    }
}

void performer_tick(void) {
    if (!img || clawd_anim_count() == 0) return;
    const uint32_t now = millis();
    switch (pst) {
    case P_WALK:
        if (walk_step(now)) {
            if (is_offstage()) { pst = P_OFF; cur = NULL; redraw(); }
            else { facing = +1; stand(); }
        }
        break;
    case P_CHOREO:
        choreo_tick(now);
        break;
    case P_SCENE: {
        if (in_loop && !loop_release && now - loop_entered >= loop_hold_ms) loop_release = true;
        if (now - frame_started < cur->holds[frame]) break;
        frame_started = now;
        const bool from_loop = in_loop;
        uint16_t next = frame + 1;
        if (frame == cur->loop_end && !loop_release) next = cur->loop_start;
        if (next >= cur->frame_count) { stand(); break; }   // outro done → idle pose
        frame = next;
        in_loop = frame >= cur->loop_start && frame <= cur->loop_end;
        if (in_loop && !from_loop) loop_entered = now;
        redraw();
        break;
    }
    default:
        break;
    }
}
