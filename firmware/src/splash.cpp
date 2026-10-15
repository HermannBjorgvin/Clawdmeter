#include "splash.h"
#include "clawd_anim.h"
#include "clawd_policy.h"
#include "splash_geometry.h"
#include "theme.h"
#include "hal/board_caps.h"
#include "hal/display_hal.h"
#include <Arduino.h>
#include <string.h>
#include <esp_heap_caps.h>

// ─── The stage room ──────────────────────────────────────────────────────────
// 60x60-cell stage; cell = min(W,H)/60 (8 px on 480, 6 on 368, 4 on 240). The
// official 55x37 art stage sits at a fixed anchor on the grid and every
// animation is placed at its authored offset, so the shared idle pose never
// jumps between animations.
#define GRID           SPLASH_GRID
#define STAGE_ANCHOR_X ((GRID - CLAWD_ART_W) / 2)
#define STAGE_ANCHOR_Y ((GRID - CLAWD_ART_H) / 2)
#define COL_EMPTY      0x0000            // true black, palette index 0

static int  cell      = 8;
static int  canvas_w  = GRID * 8;
static int  canvas_h  = GRID * 8;

LV_FONT_DECLARE(font_styrene_28);

static lv_obj_t *splash_container = NULL;
static lv_obj_t *canvas = NULL;
static lv_obj_t *label_status = NULL;     // shown only when no animations loaded
static uint16_t *canvas_buf = NULL;
static uint16_t *row_buf = NULL;
static bool active = false;
static bool occluded = false;
static int  scr_w = 480;

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
// meters act and pointing is his "noticing" gesture).
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
static int      scene_group = -1;
static bool     scene_calm = false;          // the current scene is a screensaver calm dip
static const char* last_scene = NULL;
static const char* last_show = NULL;         // never the same act twice running,
static const char* last_calm = NULL;         //   tracked per kind of scene
// walker scenes: stand → right edge → off left → home
static uint8_t  choreo_phase = 0;
static uint32_t choreo_since = 0;
static bool     leg_active = false;

static uint8_t stage_cells[GRID * GRID];
static bool    cells_any = false;            // the last composed frame shows any art

static int home_ax(const ClawdAnim* a) { return STAGE_ANCHOR_X + a->ox; }

static const uint8_t* compose(const ClawdAnim* a, uint16_t f, int ax, bool mirror) {
    memset(stage_cells, 0, sizeof(stage_cells));
    if (!a) return stage_cells;
    const int ay = STAGE_ANCHOR_Y + a->oy;
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

// ─── Two render paths ────────────────────────────────────────────────────────
// PSRAM boards draw into an LVGL canvas at native size. PSRAM-less boards
// (C6) can't hold a full canvas, so they upscale the changed cells themselves
// and push them straight to the panel via the display HAL, bypassing LVGL.
#ifndef BOARD_HAS_PSRAM
#  define SPLASH_DIRECT_DRAW 1
#else
#  define SPLASH_DIRECT_DRAW 0
#endif

#if SPLASH_DIRECT_DRAW
static uint16_t*       strip_buf = NULL;
static int             scr_cell  = 24;
static int             scr_offx  = 0;
static int             scr_offy  = 0;
static uint8_t         prev_cells[GRID * GRID];
static const uint16_t* prev_palette = NULL;
static bool            prev_valid   = false;
static bool            force_full   = false;

static void blit_cells(const uint8_t* cells, const uint16_t* palette,
                       int gx0, int gy0, int gx1, int gy1) {
    if (!strip_buf) return;
    const int spc = scr_cell;
    const int bw  = (gx1 - gx0 + 1) * spc;
    const int px  = scr_offx + gx0 * spc;
    for (int gy = gy0; gy <= gy1; gy++) {
        for (int gx = gx0; gx <= gx1; gx++) {
            uint8_t code = cells[gy * GRID + gx];
            uint16_t color = (palette && code < CLAWD_PALETTE_SIZE) ? palette[code] : COL_EMPTY;
            uint16_t* p = &strip_buf[(gx - gx0) * spc];
            for (int i = 0; i < spc; i++) p[i] = color;
        }
        for (int d = 1; d < spc; d++)
            memcpy(&strip_buf[d * bw], strip_buf, bw * 2);
        display_hal_draw_bitmap(px, scr_offy + gy * spc, bw, spc, strip_buf);
    }
}

static void render_frame(const uint8_t* cells, const uint16_t* palette) {
    if (!strip_buf || !active) return;
    if (occluded) { force_full = true; return; }   // an LVGL overlay is on top
    bool full = force_full || !prev_valid || palette != prev_palette;
    force_full = false;
    int gx0 = 0, gy0 = 0, gx1 = GRID - 1, gy1 = GRID - 1;
    if (!full) {
        gx0 = GRID; gy0 = GRID; gx1 = -1; gy1 = -1;
        for (int gy = 0; gy < GRID; gy++)
            for (int gx = 0; gx < GRID; gx++)
                if (cells[gy * GRID + gx] != prev_cells[gy * GRID + gx]) {
                    if (gx < gx0) gx0 = gx;
                    if (gx > gx1) gx1 = gx;
                    if (gy < gy0) gy0 = gy;
                    if (gy > gy1) gy1 = gy;
                }
        if (gx1 < 0) return;
    }
    blit_cells(cells, palette, gx0, gy0, gx1, gy1);
    memcpy(prev_cells, cells, GRID * GRID);
    prev_palette = palette;
    prev_valid   = true;
}
#else
static uint8_t         canvas_cells[GRID * GRID];   // what canvas_buf shows
static const uint16_t* canvas_palette = NULL;
static bool            canvas_valid   = false;

// Repaint only the cells that changed and invalidate just their bounding
// box: a full-canvas invalidate is a full-screen render + panel flush per
// stage frame, even when the frame only moves a few cells (or none: a walk
// finishing off screen).
static void render_frame(const uint8_t* cells, const uint16_t* palette) {
    if (!row_buf || !canvas_buf) return;
    const bool full = !canvas_valid || palette != canvas_palette;
    int gx0 = 0, gy0 = 0, gx1 = GRID - 1, gy1 = GRID - 1;
    if (!full) {
        gx0 = GRID; gy0 = GRID; gx1 = -1; gy1 = -1;
        for (int gy = 0; gy < GRID; gy++)
            for (int gx = 0; gx < GRID; gx++)
                if (cells[gy * GRID + gx] != canvas_cells[gy * GRID + gx]) {
                    if (gx < gx0) gx0 = gx;
                    if (gx > gx1) gx1 = gx;
                    if (gy < gy0) gy0 = gy;
                    if (gy > gy1) gy1 = gy;
                }
        if (gx1 < 0) return;
    }
    const int bw = (gx1 - gx0 + 1) * cell;
    for (int gy = gy0; gy <= gy1; gy++) {
        for (int gx = gx0; gx <= gx1; gx++) {
            uint8_t code = cells[gy * GRID + gx];
            uint16_t color = (palette && code < CLAWD_PALETTE_SIZE) ? palette[code] : COL_EMPTY;
            uint16_t* p = &row_buf[(gx - gx0) * cell];
            for (int i = 0; i < cell; i++) p[i] = color;
        }
        for (int d = 0; d < cell; d++)
            memcpy(&canvas_buf[(gy * cell + d) * canvas_w + gx0 * cell], row_buf, bw * 2);
    }
    memcpy(canvas_cells, cells, GRID * GRID);
    canvas_palette = palette;
    canvas_valid = true;
    if (!canvas) return;
    lv_area_t a;
    lv_obj_get_coords(canvas, &a);
    a.x2 = a.x1 + (gx1 + 1) * cell - 1;
    a.y2 = a.y1 + (gy1 + 1) * cell - 1;
    a.x1 += gx0 * cell;
    a.y1 += gy0 * cell;
    lv_obj_invalidate_area(canvas, &a);
}
#endif

static void render_cells(const uint8_t* cells, const uint16_t* palette) {
    cells_any = false;
    for (int i = 0; i < GRID * GRID; i++) if (cells[i]) { cells_any = true; break; }
    render_frame(cells, palette);
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

// Off-stage right: the whole frame past the right edge.
static int offstage_right_dx(void) {
    return stand_anim ? GRID - home_ax(stand_anim) : GRID;
}

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

// One step of a walk; returns true when it has finished (standing).
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
    last_scene = a->name;
    Serial.printf("[%lu] stage: scene %s\n", (unsigned long)millis(), a->name);
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
void splash_init(lv_obj_t *parent) {
    const BoardCaps& c = board_caps();
    scr_w = c.width;
    stand_anim = clawd_anim("walking");

    splash_container = lv_obj_create(parent);
    lv_obj_set_size(splash_container, c.width, c.height);
    lv_obj_set_pos(splash_container, 0, 0);
    lv_obj_set_style_bg_color(splash_container, THEME_BG, 0);
    lv_obj_set_style_bg_opa(splash_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(splash_container, 0, 0);
    lv_obj_set_style_radius(splash_container, 0, 0);
    lv_obj_set_style_pad_all(splash_container, 0, 0);
    lv_obj_clear_flag(splash_container, LV_OBJ_FLAG_SCROLLABLE);

#if SPLASH_DIRECT_DRAW
    int mind = (c.width < c.height) ? c.width : c.height;
    scr_cell = mind / GRID;
    int side = GRID * scr_cell;
    scr_offx = (c.width  - side) / 2;
    scr_offy = (c.height - side) / 2;
    strip_buf = (uint16_t*)heap_caps_malloc((size_t)side * scr_cell * 2,
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!strip_buf) Serial.println("stage: strip buffer alloc failed");
#else
    SplashGeometry geo = splash_compute_geometry(c.width, c.height, true);
    cell     = geo.cell;
    canvas_w = geo.canvas_dim;
    canvas_h = geo.canvas_dim;
    canvas_buf = (uint16_t*)heap_caps_malloc(canvas_w * canvas_h * 2, MALLOC_CAP_SPIRAM);
    row_buf    = (uint16_t*)heap_caps_malloc(canvas_w * 2,            MALLOC_CAP_SPIRAM);
    if (!canvas_buf || !row_buf) {
        Serial.println("stage: failed to alloc canvas buffer");
    } else {
        canvas = lv_canvas_create(splash_container);
        lv_canvas_set_buffer(canvas, canvas_buf, canvas_w, canvas_h, LV_COLOR_FORMAT_RGB565);
        // Absolute position, not centred: the streak narrows the container
        // and a centred child would slide with it.
        lv_obj_set_pos(canvas, (c.width - canvas_w) / 2, (c.height - canvas_h) / 2);
    }
#endif

    label_status = lv_label_create(splash_container);
    lv_label_set_text(label_status, "no animations loaded\n\nrun tools/convert_official_clawd.js");
    lv_obj_set_style_text_font(label_status, &font_styrene_28, 0);
    lv_obj_set_style_text_color(label_status, lv_color_hex(0xb0aea5), 0);
    lv_obj_set_style_text_align(label_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label_status);
    if (clawd_anim_count() > 0) lv_obj_add_flag(label_status, LV_OBJ_FLAG_HIDDEN);

    pst = P_OFF;
    redraw();
    lv_obj_add_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
}

// ─── Room ────────────────────────────────────────────────────────────────────
void splash_open(void) {
    if (!splash_container) return;
    lv_obj_set_width(splash_container, scr_w);
    lv_obj_clear_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
    active = true;
#if SPLASH_DIRECT_DRAW
    // LVGL paints the black container on its next refresh, which would erase
    // anything drawn now; splash_tick repaints once that has happened.
    force_full = true;
#else
    redraw();
#endif
}

void splash_close(void) {
    if (splash_container) {
        lv_obj_add_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(splash_container, scr_w);
    }
    active = false;
    pst = P_OFF;
    cur = NULL;
    dx = offstage_right_dx();
}

bool splash_is_active(void) { return active; }
lv_obj_t* splash_get_root(void) { return splash_container; }

void splash_set_occluded(bool o) {
    if (o == occluded) return;
    occluded = o;
#if SPLASH_DIRECT_DRAW
    if (!o && active) {
        // Flush the overlay's last LVGL frame first, or it would paint the
        // stage container's black over the repaint below.
        lv_refr_now(NULL);
        force_full = true;
        redraw();
    }
#endif
}

// ─── Performer API ───────────────────────────────────────────────────────────
void splash_place_home(void) { dx = 0; facing = +1; stand(); }

void splash_place_offstage_right(void) {
    dx = offstage_right_dx();
    facing = -1;
    pst = P_OFF;
    cur = NULL;
    redraw();
}

void splash_walk_home(uint16_t spd) {
    if (!stand_anim) return;
    if (dx == 0) { stand(); return; }
    pst = P_WALK;
    walk_start(stand_anim, GAIT_WALK, 0, spd);
}

void splash_walk_off_right(uint16_t spd) {
    if (!stand_anim) return;
    pst = P_WALK;
    walk_start(stand_anim, GAIT_WALK, offstage_right_dx(), spd);
}

void splash_run_off_right(uint16_t from_pct, uint16_t to_pct, uint32_t ramp) {
    if (!stand_anim) return;
    pst = P_WALK;
    walk_start(stand_anim, GAIT_WALK, offstage_right_dx(), from_pct);
    ramp_from = from_pct;
    ramp_to = to_pct;
    ramp_ms = ramp;
}

void splash_perform(int idx, uint32_t loop_ms) {
    scene_group = -1;
    scene_calm = false;
    scene_start(clawd_anim_at(idx), loop_ms);
}

static ClawdRng rng;
static bool rng_seeded = false;

void splash_perform_group(int group, uint32_t loop_ms, bool calm) {
    if (!rng_seeded) { clawd_rng_seed(&rng, millis() ^ 0x57A6E); rng_seeded = true; }
    if (group < 0 || group >= GROUP_COUNT) group = 0;
    const char* const* names = calm ? CALM_NAMES : GROUP_NAMES[group];
    const int n = calm ? (int)(sizeof(CALM_NAMES) / sizeof(CALM_NAMES[0])) : GROUP_MAX;
    const char*& last = calm ? last_calm : last_show;
    const int pick = clawd_pick_no_repeat(names, n, last, &rng);
    scene_group = calm ? 0 : group;
    scene_calm = calm;
    const ClawdAnim* a = pick >= 0 ? clawd_anim(names[pick]) : NULL;
    if (a) last = a->name;
    scene_start(a, loop_ms);
}

void splash_release(void) {
    if (pst == P_OFF) return;
    // Snap to the idle pose where he stands (scenes are authored at home).
    if (pst == P_SCENE) dx = 0;
    facing = +1;
    stand();
}

bool splash_performer_visible(void) { return active && cells_any; }

bool splash_busy(void) { return pst == P_WALK || pst == P_SCENE || pst == P_CHOREO; }

void splash_next(void) {
    if (pst != P_SCENE && pst != P_CHOREO) return;
    // Step within the list the scene came from: a calm dip only ever steps to
    // another calm scene (the screensaver stays calm), a show within its group.
    const int g = scene_group >= 0 ? scene_group : 0;
    const char* const* names = scene_calm ? CALM_NAMES : GROUP_NAMES[g];
    const int n = scene_calm ? (int)(sizeof(CALM_NAMES) / sizeof(CALM_NAMES[0])) : GROUP_MAX;
    int at = 0;
    for (int i = 0; i < n; i++)
        if (last_scene && names[i] && strcmp(names[i], last_scene) == 0) at = i;
    for (int k = 1; k <= n; k++) {
        const ClawdAnim* a = clawd_anim(names[(at + k) % n]);
        if (a && strcmp(a->name, "lurking") != 0) {
            (scene_calm ? last_calm : last_show) = a->name;
            scene_start(a, loop_hold_ms);
            return;
        }
    }
}

// ─── Tick ────────────────────────────────────────────────────────────────────
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
        if (now - choreo_since > 1200) { choreo_phase = 1; choreo_leg((GRID - a->w) - home_ax(a)); }
        break;
    case 2:  // at the right edge
        if (now - choreo_since > 1200) { choreo_phase = 3; choreo_leg(-a->w - home_ax(a)); }
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

void splash_tick(void) {
    if (!active || clawd_anim_count() == 0) return;
    const uint32_t now = millis();

#if SPLASH_DIRECT_DRAW
    if (force_full && !occluded) redraw();
#endif

    switch (pst) {
    case P_WALK:
        if (walk_step(now)) { facing = dx > 0 && dx >= offstage_right_dx() ? -1 : +1;
            if (dx >= offstage_right_dx()) { pst = P_OFF; cur = NULL; redraw(); }
            else stand(); }
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
