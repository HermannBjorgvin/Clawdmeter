#include "splash.h"
#include "splash_animations.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

// 20x20 grid scaled 24x to fill 480x480
#define GRID         20
#define CELL         24
#define CANVAS_W     (GRID * CELL)
#define CANVAS_H     (GRID * CELL)

// Background fallback when palette is missing
#define COL_EMPTY    0x10A2  // dark bg (#0f0f0f)

LV_FONT_DECLARE(font_styrene_28);

static lv_obj_t *splash_container = NULL;
static lv_obj_t *canvas = NULL;
static lv_obj_t *label_status = NULL;     // shown only when no animations loaded
static uint16_t *canvas_buf = NULL;        // 480x480 RGB565 (PSRAM)

static uint16_t cur_anim = 0;
static uint16_t cur_frame = 0;
static uint32_t frame_started_ms = 0;
static bool active = false;

static void render_frame(const uint8_t *cells, const uint16_t *palette) {
    for (int gy = 0; gy < GRID; gy++) {
        uint16_t row[CANVAS_W];
        for (int gx = 0; gx < GRID; gx++) {
            uint8_t code = cells[gy * GRID + gx];
            uint16_t color = (palette && code < SPLASH_PALETTE_SIZE) ? palette[code] : COL_EMPTY;
            uint16_t *p = &row[gx * CELL];
            for (int i = 0; i < CELL; i++) p[i] = color;
        }
        for (int dy = 0; dy < CELL; dy++) {
            memcpy(&canvas_buf[(gy * CELL + dy) * CANVAS_W], row, CANVAS_W * 2);
        }
    }
    if (canvas) lv_obj_invalidate(canvas);
}

static void show_placeholder() {
    // Solid dark background + centered status label.
    for (int i = 0; i < CANVAS_W * CANVAS_H; i++) canvas_buf[i] = COL_EMPTY;
    if (canvas) lv_obj_invalidate(canvas);
    if (label_status) lv_obj_clear_flag(label_status, LV_OBJ_FLAG_HIDDEN);
}

void splash_init(lv_obj_t *parent) {
    canvas_buf = (uint16_t*)heap_caps_malloc(CANVAS_W * CANVAS_H * 2, MALLOC_CAP_SPIRAM);
    if (!canvas_buf) {
        Serial.println("splash: failed to alloc canvas buffer");
        return;
    }

    splash_container = lv_obj_create(parent);
    lv_obj_set_size(splash_container, 480, 480);
    lv_obj_set_pos(splash_container, 0, 0);
    lv_obj_set_style_bg_color(splash_container, lv_color_hex(0x0f0f0f), 0);
    lv_obj_set_style_bg_opa(splash_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(splash_container, 0, 0);
    lv_obj_set_style_pad_all(splash_container, 0, 0);
    lv_obj_clear_flag(splash_container, LV_OBJ_FLAG_SCROLLABLE);

    canvas = lv_canvas_create(splash_container);
    lv_canvas_set_buffer(canvas, canvas_buf, CANVAS_W, CANVAS_H, LV_COLOR_FORMAT_RGB565);
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
    const splash_anim_def_t *a = &splash_anims[cur_anim];
    render_frame(a->frames[0], a->palette);
    Serial.printf("splash: -> %s\n", a->name);
}

void splash_prev(void) {
    if (SPLASH_ANIM_COUNT == 0) return;
    cur_anim = (cur_anim + SPLASH_ANIM_COUNT - 1) % SPLASH_ANIM_COUNT;
    cur_frame = 0;
    frame_started_ms = millis();
    const splash_anim_def_t *a = &splash_anims[cur_anim];
    render_frame(a->frames[0], a->palette);
    Serial.printf("splash: <- %s\n", a->name);
}

void splash_set_active(bool a) { active = a; }

void splash_show(void) {
    if (splash_container) lv_obj_clear_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
    active = true;
}

void splash_hide(void) {
    if (splash_container) lv_obj_add_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
    active = false;
}

const char *splash_current_name(void) {
    if (SPLASH_ANIM_COUNT == 0) return "(none)";
    return splash_anims[cur_anim].name;
}

uint16_t splash_count(void) { return SPLASH_ANIM_COUNT; }
