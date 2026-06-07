#include "splash.h"
#include "splash_animations.h"
#include "theme.h"
#include "usage_rate.h"
#include "hal/board_caps.h"
#include <Arduino.h>
#include <string.h>
#include <esp_heap_caps.h>

// 20×20 grid. CELL sized so the canvas fits the smaller display dimension —
// the canvas is square and centered, so on portrait or letterboxed panels
// it leaves vertical margin rather than cropping.
#define GRID         20
static int  cell      = 24;        // recomputed in splash_init()
static int  canvas_w  = GRID * 24;
static int  canvas_h  = GRID * 24;

// Background fallback when palette is missing
#define COL_EMPTY    0x0000  // true black (matches THEME_BG)

LV_FONT_DECLARE(font_styrene_28);

static lv_obj_t *splash_container = NULL;
static lv_obj_t *canvas = NULL;
static lv_obj_t *label_status = NULL;     // shown only when no animations loaded
static uint16_t *canvas_buf = NULL;        // 480x480 RGB565 (PSRAM)

static uint16_t cur_anim = 0;
static uint16_t cur_frame = 0;
static uint32_t frame_started_ms = 0;
static uint32_t last_pick_ms = 0;
static bool active = false;

// Working-mode state: while Claude Code runs, "home" to the "work coding"
// animation; allow a temporary manual cycle that snaps back after a few seconds.
static bool     working_mode    = false;
static uint32_t last_manual_ms  = 0;
static bool     manual_override = false;   // user cycled away from "work coding"
static int      work_coding_idx = -1;      // resolved once in splash_init()
#define SPLASH_WORK_REVERT_MS 5000

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

static uint16_t *row_buf = NULL;   // scratch row, sized to canvas_w

static void render_frame(const uint8_t *cells, const uint16_t *palette) {
    if (!row_buf || !canvas_buf) return;
    for (int gy = 0; gy < GRID; gy++) {
        for (int gx = 0; gx < GRID; gx++) {
            uint8_t code = cells[gy * GRID + gx];
            uint16_t color = (palette && code < SPLASH_PALETTE_SIZE) ? palette[code] : COL_EMPTY;
            uint16_t *p = &row_buf[gx * cell];
            for (int i = 0; i < cell; i++) p[i] = color;
        }
        for (int dy = 0; dy < cell; dy++) {
            memcpy(&canvas_buf[(gy * cell + dy) * canvas_w], row_buf, canvas_w * 2);
        }
    }
    if (canvas) lv_obj_invalidate(canvas);
}

// ---- Mini creature: a small animated creature for embedding in other screens
//      (e.g. the idle "sleeping" indicator, or a "working" badge on the usage
//      meter). Each instance is caller-owned (splash_mini_t) so several can run
//      at once, independent of the full-screen splash above. ----
static void mini_render(splash_mini_t *m) {
    if (!m->buf || m->anim_idx < 0) return;
    const splash_anim_def_t *a = &splash_anims[m->anim_idx];
    const uint8_t *cells = a->frames[m->frame];
    const uint16_t *pal = a->palette;
    for (int gy = 0; gy < GRID; gy++) {
        for (int gx = 0; gx < GRID; gx++) {
            uint8_t code = cells[gy * GRID + gx];
            uint16_t color = (pal && code < SPLASH_PALETTE_SIZE) ? pal[code] : COL_EMPTY;
            for (int dy = 0; dy < m->cell; dy++) {
                uint16_t *dst = &m->buf[(gy * m->cell + dy) * m->w + gx * m->cell];
                for (int dx = 0; dx < m->cell; dx++) dst[dx] = color;
            }
        }
    }
    if (m->canvas) lv_obj_invalidate(m->canvas);
}

lv_obj_t* splash_mini_create(splash_mini_t *m, lv_obj_t *parent, const char *anim_name, int px) {
    m->anim_idx = -1;
    m->buf = NULL;
    m->canvas = NULL;
    m->frame = 0;
    for (int i = 0; i < SPLASH_ANIM_COUNT; i++) {
        if (strcmp(splash_anims[i].name, anim_name) == 0) { m->anim_idx = i; break; }
    }
    if (m->anim_idx < 0) return NULL;
    m->cell = px / GRID;
    if (m->cell < 1) m->cell = 1;
    m->w = GRID * m->cell;
#ifdef BOARD_HAS_PSRAM
    const uint32_t caps = MALLOC_CAP_SPIRAM;
#else
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif
    m->buf = (uint16_t*)heap_caps_malloc(m->w * m->w * 2, caps);
    if (!m->buf) { m->anim_idx = -1; return NULL; }
    m->canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(m->canvas, m->buf, m->w, m->w, LV_COLOR_FORMAT_RGB565);
    m->started = millis();
    mini_render(m);
    return m->canvas;
}

void splash_mini_tick(splash_mini_t *m) {
    if (!m->buf || m->anim_idx < 0) return;
    const splash_anim_def_t *a = &splash_anims[m->anim_idx];
    if (a->frame_count == 0) return;
    if (millis() - m->started < a->holds[m->frame]) return;
    m->started = millis();
    m->frame = (m->frame + 1) % a->frame_count;
    mini_render(m);
}

static void show_placeholder() {
    // Solid dark background + centered status label.
    if (canvas_buf) {
        for (int i = 0; i < canvas_w * canvas_h; i++) canvas_buf[i] = COL_EMPTY;
    }
    if (canvas) lv_obj_invalidate(canvas);
    if (label_status) lv_obj_clear_flag(label_status, LV_OBJ_FLAG_HIDDEN);
}

void splash_init(lv_obj_t *parent) {
    const BoardCaps& c = board_caps();
    int min_dim = (c.width < c.height) ? c.width : c.height;
    cell     = min_dim / GRID;       // fits within the smaller display dimension
    if (cell < 4) cell = 4;

#ifdef BOARD_HAS_PSRAM
    const uint32_t canvas_caps = MALLOC_CAP_SPIRAM;
#else
    // Without PSRAM the full 480×480 RGB565 canvas (460 KB) won't fit. Cap
    // the canvas so the buffer stays under ~80 KB, leaving the rest of
    // internal SRAM free for LVGL, NimBLE, and the audio/PMU stacks. The
    // canvas is centered, so the cost is extra black border around the
    // pixel art — not cropping.
    const uint32_t canvas_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const int MAX_CELL_NO_PSRAM = 10;  // 10*20=200; 200*200*2=78 KB
    if (cell > MAX_CELL_NO_PSRAM) cell = MAX_CELL_NO_PSRAM;
#endif

    canvas_w = GRID * cell;
    canvas_h = GRID * cell;

    canvas_buf = (uint16_t*)heap_caps_malloc(canvas_w * canvas_h * 2, canvas_caps);
    row_buf    = (uint16_t*)heap_caps_malloc(canvas_w * 2,             canvas_caps);
    if (!canvas_buf || !row_buf) {
        Serial.println("splash: failed to alloc canvas buffer");
        return;
    }

    splash_container = lv_obj_create(parent);
    lv_obj_set_size(splash_container, c.width, c.height);
    lv_obj_set_pos(splash_container, 0, 0);
    lv_obj_set_style_bg_color(splash_container, THEME_BG, 0);
    lv_obj_set_style_bg_opa(splash_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(splash_container, 0, 0);
    lv_obj_set_style_pad_all(splash_container, 0, 0);
    lv_obj_clear_flag(splash_container, LV_OBJ_FLAG_SCROLLABLE);

    canvas = lv_canvas_create(splash_container);
    lv_canvas_set_buffer(canvas, canvas_buf, canvas_w, canvas_h, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(canvas);

    // Placeholder label (visible only when no animations are loaded)
    label_status = lv_label_create(splash_container);
    lv_label_set_text(label_status,
        "no animations loaded\n\n"
        "run tools/scrape_claudepix.js\n"
        "then tools/convert_to_c.js");
    lv_obj_set_style_text_font(label_status, &font_styrene_28, 0);
    lv_obj_set_style_text_color(label_status, lv_color_hex(0xb0aea5), 0);
    lv_obj_set_style_text_align(label_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label_status);

    resolve_group_lists();

    work_coding_idx = -1;
    for (int i = 0; i < SPLASH_ANIM_COUNT; i++) {
        if (strcmp(splash_anims[i].name, "work coding") == 0) { work_coding_idx = i; break; }
    }

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

// Force a specific animation by catalog index (used by working-mode).
static void splash_show_index(int idx) {
    if (idx < 0 || idx >= SPLASH_ANIM_COUNT) return;
    cur_anim = (uint16_t)idx;
    cur_frame = 0;
    frame_started_ms = millis();
    last_pick_ms = frame_started_ms;
    const splash_anim_def_t *a = &splash_anims[cur_anim];
    render_frame(a->frames[0], a->palette);
}

void splash_tick(void) {
    if (!active || SPLASH_ANIM_COUNT == 0) return;

    if (working_mode) {
        // Stay on "work coding"; a manual PWR cycle is allowed but snaps back.
        if (manual_override && (millis() - last_manual_ms >= SPLASH_WORK_REVERT_MS)) {
            manual_override = false;
            if (work_coding_idx >= 0) splash_show_index(work_coding_idx);
        }
    } else if (millis() - last_pick_ms >= SPLASH_ROTATE_INTERVAL_MS) {
        // Auto-rotate to the next animation in the current rate group.
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
    splash_note_manual();
}

void splash_pick_for_current_rate(void) {
    if (SPLASH_ANIM_COUNT == 0) return;
    int g = usage_rate_group();
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
    if (working_mode && work_coding_idx >= 0) {
        splash_show_index(work_coding_idx);
        manual_override = false;
    } else {
        splash_pick_for_current_rate();
    }
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

void splash_set_working(bool working) {
    if (working == working_mode) return;
    working_mode = working;
    manual_override = false;
    if (working) {
        if (active && work_coding_idx >= 0) splash_show_index(work_coding_idx);
    } else {
        last_pick_ms = millis();  // don't instantly auto-rotate when resuming
    }
}

void splash_note_manual(void) {
    if (!working_mode) return;
    manual_override = true;
    last_manual_ms = millis();
}

bool splash_get_working(void) { return working_mode; }
