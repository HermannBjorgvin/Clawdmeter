#include "splash.h"
#include "splash_animations.h"
#include "theme.h"
#include "usage_rate.h"
#include "display_cfg.h"
#include <Arduino.h>
#include <string.h>

// 20x20 grid scaled to fit display width (CELL = floor(LCD_WIDTH / GRID))
#define GRID         20
#define CELL         (LCD_WIDTH / GRID)   // 135/20 = 6 -> 120x120 canvas
#define CANVAS_W     (GRID * CELL)
#define CANVAS_H     (GRID * CELL)

// Background fallback when palette is missing
#define COL_EMPTY    0x0000  // true black (matches THEME_BG)

LV_FONT_DECLARE(font_styrene_14);

static lv_obj_t *splash_container = NULL;
static lv_obj_t *canvas = NULL;
static lv_obj_t *label_status = NULL;     // shown only when no animations loaded

// Splash, Copilot and Aurora canvases are never visible at the same time
// (mutually exclusive screens in the cycle), so they share ONE buffer sized
// to the largest of the three (this splash canvas, 120x120) instead of each
// permanently holding their own — saves ~23 KB of heap that's otherwise
// never used concurrently. Whichever screen is shown re-renders its first
// frame into it immediately (see the _show() functions below), so stale
// content from a previous screen never stays visible past that.
static uint16_t *shared_canvas_buf = NULL;
#define SHARED_CANVAS_BUF_BYTES (CANVAS_W * CANVAS_H * 2)

static uint16_t *get_shared_canvas_buf(void) {
    if (!shared_canvas_buf) {
        shared_canvas_buf = (uint16_t*)malloc(SHARED_CANVAS_BUF_BYTES);
        if (shared_canvas_buf) memset(shared_canvas_buf, 0, SHARED_CANVAS_BUF_BYTES);
    }
    return shared_canvas_buf;
}

static uint16_t cur_anim = 0;
static uint16_t cur_frame = 0;
static uint32_t frame_started_ms = 0;
static uint32_t last_pick_ms = 0;
static bool active = false;
static int  activity_group = -1;  // Claude-activity override; -1 = use usage rate

// While splash is showing, auto-cycle to the next animation in the current
// rate-driven group every this many ms.
#define SPLASH_ROTATE_INTERVAL_MS 20000

// Usage-rate animation groups: 4 groups × up to 4 animations each.
// Filled at init by matching literal names from splash_anims[].
#define GROUP_COUNT 4
#define GROUP_MAX   4
static int8_t  group_lists[GROUP_COUNT][GROUP_MAX];
static uint8_t group_size[GROUP_COUNT] = {0};
static uint8_t group_rotation[GROUP_COUNT] = {0};

static const char* GROUP_NAMES[GROUP_COUNT][GROUP_MAX] = {
    // Group 0 — idle / sleepy
    { "expression sleep", "idle breathe", "idle blink", "expression wink" },
    // Group 1 — normal pace
    { "idle look around", "work think", "work coding", NULL },
    // Group 2 — active
    { "dance sway", "expression surprise", "dance bounce", NULL },
    // Group 3 — heavy
    { "dance bounce dj", "dance sway dj", "dance djmix", NULL },
};

static void resolve_group_lists(void) {
    for (int g = 0; g < GROUP_COUNT; g++) {
        group_size[g] = 0;
        for (int s = 0; s < GROUP_MAX; s++) {
            group_lists[g][s] = -1;
            const char* want = GROUP_NAMES[g][s];
            if (!want) continue;
            for (int i = 0; i < SPLASH_ANIM_COUNT; i++) {
                if (strcmp(splash_anims[i].name, want) == 0) {
                    group_lists[g][group_size[g]++] = (int8_t)i;
                    break;
                }
            }
        }
    }
}

// Generic renderer — draws any 20×20 frame into an arbitrary canvas buffer.
// canvas_w must equal GRID * cell. Invalidates cvs when done.
static void render_frame_to(uint16_t* buf, lv_obj_t* cvs, int canvas_w, int cell,
                             const uint8_t* cells, const uint16_t* palette) {
    if (!buf) return;  // the canvas's malloc() failed (low heap) — degrade, don't crash
    uint16_t row[GRID * 6];  // max cell=6 → 120 pixels
    for (int gy = 0; gy < GRID; gy++) {
        for (int gx = 0; gx < GRID; gx++) {
            uint8_t code = cells[gy * GRID + gx];
            uint16_t color = (palette && code < SPLASH_PALETTE_SIZE) ? palette[code] : COL_EMPTY;
            uint16_t *p = &row[gx * cell];
            for (int i = 0; i < cell; i++) p[i] = color;
        }
        for (int dy = 0; dy < cell; dy++) {
            memcpy(&buf[(gy * cell + dy) * canvas_w], row, canvas_w * 2);
        }
    }
    if (cvs) lv_obj_invalidate(cvs);
}

static void render_frame(const uint8_t *cells, const uint16_t *palette) {
    render_frame_to(shared_canvas_buf, canvas, CANVAS_W, CELL, cells, palette);
}

// ---- Copilot canvas (60x60 @ 3x scale) ----
#define COPILOT_CELL     3
#define COPILOT_CANVAS_W (GRID * COPILOT_CELL)   // 60
#define COPILOT_CANVAS_H (GRID * COPILOT_CELL)   // 60

static lv_obj_t*  copilot_canvas     = NULL;
static int16_t    copilot_anim_idx   = -1;  // index into splash_anims[]
static uint16_t   copilot_frame_idx  = 0;
static uint32_t   copilot_frame_ms   = 0;

void splash_copilot_init(lv_obj_t* parent) {
    uint16_t* buf = get_shared_canvas_buf();
    if (!buf) {
        Serial.println("splash: copilot canvas alloc failed");
        return;
    }

    copilot_canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(copilot_canvas, buf,
                         COPILOT_CANVAS_W, COPILOT_CANVAS_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(copilot_canvas, LV_ALIGN_BOTTOM_MID, 0, -4);

    // Find "copilot idle" animation by name
    copilot_anim_idx = -1;
    for (int i = 0; i < SPLASH_ANIM_COUNT; i++) {
        if (strcmp(splash_anims[i].name, "copilot idle") == 0) {
            copilot_anim_idx = (int16_t)i;
            break;
        }
    }
    if (copilot_anim_idx < 0) {
        Serial.println("splash: 'copilot idle' animation not found");
        return;
    }
    // Don't render yet — the shared buffer may still be owned by whichever
    // canvas initializes after this one; splash_copilot_show() (called by
    // ui_show_screen() on entry to the Copilot screen) renders the first
    // frame fresh right before this canvas actually becomes visible.
    copilot_frame_idx = 0;
}

// Re-renders the current frame fresh into the shared buffer. Call whenever
// the Copilot screen becomes visible, since another screen may have
// overwritten the shared buffer with its own content since we last drew.
void splash_copilot_show(void) {
    if (copilot_anim_idx < 0 || !shared_canvas_buf) return;
    copilot_frame_idx = 0;
    copilot_frame_ms  = millis();
    const splash_anim_def_t* a = &splash_anims[copilot_anim_idx];
    render_frame_to(shared_canvas_buf, copilot_canvas,
                    COPILOT_CANVAS_W, COPILOT_CELL, a->frames[0], a->palette);
}

void splash_copilot_tick(void) {
    if (copilot_anim_idx < 0 || !shared_canvas_buf) return;
    const splash_anim_def_t* a = &splash_anims[copilot_anim_idx];
    if (a->frame_count == 0) return;
    if (millis() - copilot_frame_ms >= a->holds[copilot_frame_idx]) {
        copilot_frame_idx = (copilot_frame_idx + 1) % a->frame_count;
        copilot_frame_ms  = millis();
        render_frame_to(shared_canvas_buf, copilot_canvas,
                        COPILOT_CANVAS_W, COPILOT_CELL,
                        a->frames[copilot_frame_idx], a->palette);
    }
}

// ---- Aurora canvas — full screen width, 60px tall ----
// Unlike the Copilot canvas (uniform square cells), Aurora stretches the 20
// grid columns horizontally to fill LCD_WIDTH exactly while keeping a fixed
// 3px row height — the animation reads as a full-width "curtain" strip
// rather than a centered square. See render_frame_stretched().
#define AURORA_CELL_H    3
#define AURORA_CANVAS_W  LCD_WIDTH                // 135 — full screen width
#define AURORA_CANVAS_H  (GRID * AURORA_CELL_H)    // 60 — unchanged

static lv_obj_t*  aurora_canvas     = NULL;
static int16_t    aurora_anim_idx   = -1;  // index into splash_anims[]
static uint16_t   aurora_frame_idx  = 0;
static uint32_t   aurora_frame_ms   = 0;

// Renders a GRID-wide frame stretched horizontally to fill canvas_w exactly
// (columns get 6 or 7 px each at 135/20, via integer-division boundaries —
// no gaps/overlaps), with a fixed pixel height per row (cell_h). Used where
// the target aspect ratio doesn't match a uniform square-cell scale.
static void render_frame_stretched(uint16_t* buf, lv_obj_t* cvs, int canvas_w, int cell_h,
                                    const uint8_t* cells, const uint16_t* palette) {
    if (!buf) return;  // the canvas's malloc() failed (low heap) — degrade, don't crash
    uint16_t row[LCD_WIDTH];
    for (int gy = 0; gy < GRID; gy++) {
        int col = 0;
        for (int gx = 0; gx < GRID; gx++) {
            uint8_t code = cells[gy * GRID + gx];
            uint16_t color = (palette && code < SPLASH_PALETTE_SIZE) ? palette[code] : COL_EMPTY;
            int x_end = (gx + 1) * canvas_w / GRID;
            while (col < x_end) row[col++] = color;
        }
        for (int dy = 0; dy < cell_h; dy++) {
            memcpy(&buf[(gy * cell_h + dy) * canvas_w], row, canvas_w * 2);
        }
    }
    if (cvs) lv_obj_invalidate(cvs);
}

void splash_aurora_init(lv_obj_t* parent) {
    uint16_t* buf = get_shared_canvas_buf();
    if (!buf) {
        Serial.println("splash: aurora canvas alloc failed");
        return;
    }

    aurora_canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(aurora_canvas, buf,
                         AURORA_CANVAS_W, AURORA_CANVAS_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(aurora_canvas, LV_ALIGN_TOP_MID, 0, 30);

    aurora_anim_idx = -1;
    for (int i = 0; i < SPLASH_ANIM_COUNT; i++) {
        if (strcmp(splash_anims[i].name, "aurora shimmer") == 0) {
            aurora_anim_idx = (int16_t)i;
            break;
        }
    }
    if (aurora_anim_idx < 0) {
        Serial.println("splash: 'aurora shimmer' animation not found");
        return;
    }
    // Don't render yet — see the comment in splash_copilot_init(); the
    // shared buffer is only actually drawn into once this screen is shown.
    aurora_frame_idx = 0;
}

// Re-renders the current frame fresh into the shared buffer. Call whenever
// the Aurora screen becomes visible (see splash_copilot_show()'s comment).
void splash_aurora_show(void) {
    if (aurora_anim_idx < 0 || !shared_canvas_buf) return;
    aurora_frame_idx = 0;
    aurora_frame_ms  = millis();
    const splash_anim_def_t* a = &splash_anims[aurora_anim_idx];
    render_frame_stretched(shared_canvas_buf, aurora_canvas,
                           AURORA_CANVAS_W, AURORA_CELL_H, a->frames[0], a->palette);
}

void splash_aurora_tick(void) {
    if (aurora_anim_idx < 0 || !shared_canvas_buf) return;
    const splash_anim_def_t* a = &splash_anims[aurora_anim_idx];
    if (a->frame_count == 0) return;
    if (millis() - aurora_frame_ms >= a->holds[aurora_frame_idx]) {
        aurora_frame_idx = (aurora_frame_idx + 1) % a->frame_count;
        aurora_frame_ms  = millis();
        render_frame_stretched(shared_canvas_buf, aurora_canvas,
                               AURORA_CANVAS_W, AURORA_CELL_H,
                               a->frames[aurora_frame_idx], a->palette);
    }
}

static void show_placeholder() {
    // Solid dark background + centered status label.
    if (shared_canvas_buf) {
        for (int i = 0; i < CANVAS_W * CANVAS_H; i++) shared_canvas_buf[i] = COL_EMPTY;
        if (canvas) lv_obj_invalidate(canvas);
    }
    if (label_status) lv_obj_clear_flag(label_status, LV_OBJ_FLAG_HIDDEN);
}

void splash_init(lv_obj_t *parent) {
    uint16_t* init_buf = get_shared_canvas_buf();
    if (!init_buf) {
        Serial.println("splash: failed to alloc canvas buffer");
        return;
    }

    splash_container = lv_obj_create(parent);
    lv_obj_set_size(splash_container, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_pos(splash_container, 0, 0);
    lv_obj_set_style_bg_color(splash_container, THEME_BG, 0);
    lv_obj_set_style_bg_opa(splash_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(splash_container, 0, 0);
    lv_obj_set_style_pad_all(splash_container, 0, 0);
    lv_obj_clear_flag(splash_container, LV_OBJ_FLAG_SCROLLABLE);

    canvas = lv_canvas_create(splash_container);
    lv_canvas_set_buffer(canvas, init_buf, CANVAS_W, CANVAS_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(canvas);

    // Placeholder label (visible only when no animations are loaded)
    label_status = lv_label_create(splash_container);
    lv_label_set_text(label_status,
        "no animations loaded\n\n"
        "run tools/scrape_claudepix.js\n"
        "then tools/convert_to_c.js");
    lv_obj_set_style_text_font(label_status, &font_styrene_14, 0);
    lv_obj_set_style_text_color(label_status, lv_color_hex(0xb0aea5), 0);
    lv_obj_set_style_text_align(label_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label_status);

    resolve_group_lists();

    if (SPLASH_ANIM_COUNT == 0) {
        show_placeholder();
    } else {
        lv_obj_add_flag(label_status, LV_OBJ_FLAG_HIDDEN);
        const splash_anim_def_t *a = &splash_anims[0];
        render_frame(a->frames[0], a->palette);
        frame_started_ms = millis();
    }

    lv_obj_add_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
}

void splash_tick(void) {
    if (!active || SPLASH_ANIM_COUNT == 0) return;

    // Auto-rotate to the next animation in the current group.
    if (millis() - last_pick_ms >= SPLASH_ROTATE_INTERVAL_MS) {
        splash_pick_for_current_rate();
    }

    const splash_anim_def_t *a = &splash_anims[cur_anim];
    if (a->frame_count == 0) return;

    uint16_t hold = a->holds[cur_frame];
    if (millis() - frame_started_ms >= hold) {
        cur_frame = (cur_frame + 1) % a->frame_count;
        frame_started_ms = millis();
        render_frame(a->frames[cur_frame], a->palette);
    }
}

void splash_next(void) {
    if (SPLASH_ANIM_COUNT == 0) return;
    cur_anim = (cur_anim + 1) % SPLASH_ANIM_COUNT;
    cur_frame = 0;
    frame_started_ms = millis();
    last_pick_ms = frame_started_ms;
    const splash_anim_def_t *a = &splash_anims[cur_anim];
    render_frame(a->frames[0], a->palette);
    Serial.printf("splash: -> %s\n", a->name);
}

void splash_set_activity(int group) {
    if (group == activity_group) return;
    activity_group = (group >= 0 && group < GROUP_COUNT) ? group : -1;
    if (active) splash_pick_for_current_rate();
}

void splash_pick_for_current_rate(void) {
    if (SPLASH_ANIM_COUNT == 0) return;
    int g = (activity_group >= 0) ? activity_group : usage_rate_group();
    if (g < 0 || g >= GROUP_COUNT) g = 0;
    if (group_size[g] == 0) return;

    uint8_t slot = group_rotation[g] % group_size[g];
    group_rotation[g]++;
    int8_t idx = group_lists[g][slot];
    if (idx < 0) return;

    cur_anim = (uint16_t)idx;
    cur_frame = 0;
    frame_started_ms = millis();
    last_pick_ms = frame_started_ms;
    const splash_anim_def_t *a = &splash_anims[cur_anim];
    render_frame(a->frames[0], a->palette);
}

bool splash_is_active(void) { return active; }

void splash_show(void) {
    splash_pick_for_current_rate();
    if (splash_container) lv_obj_clear_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
    active = true;
}

void splash_hide(void) {
    if (splash_container) lv_obj_add_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
    active = false;
}

lv_obj_t* splash_get_root(void) {
    return splash_container;
}
