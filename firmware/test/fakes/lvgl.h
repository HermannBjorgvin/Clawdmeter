#pragma once
// Host stand-in for the few LVGL types and calls the director touches, for
// the director test (test/test_director), and LVGL's font descriptors, so
// test_layout can compile the real font_*.c files and measure text with
// them (test_layout/lv_font_host.c implements the two font calls).
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
typedef struct _lv_obj_t   lv_obj_t;
typedef struct _lv_indev_t lv_indev_t;
typedef struct _lv_event_t lv_event_t;
typedef struct _lv_display_t lv_display_t;
typedef struct { int32_t x, y; } lv_point_t;
typedef void (*lv_event_cb_t)(lv_event_t*);
enum { LV_EVENT_PRESSED = 1 };

static inline lv_indev_t* lv_indev_get_next(lv_indev_t*) { return (lv_indev_t*)0; }
static inline void lv_indev_add_event_cb(lv_indev_t*, lv_event_cb_t, int, void*) {}
static inline void lv_indev_get_point(lv_indev_t*, lv_point_t* p) { p->x = 0; p->y = 0; }
static inline int32_t lv_display_get_horizontal_resolution(lv_display_t*) { return 480; }

extern "C" {
#endif

// ---- Fonts (field names and order as LVGL 9's lv_font_fmt_txt.h) ----
#define LVGL_VERSION_MAJOR 9
#define LV_ATTRIBUTE_LARGE_CONST
#define LV_FONT_DECLARE(name) extern const lv_font_t name
enum { LV_FONT_SUBPX_NONE = 0 };
enum {
    LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL,
    LV_FONT_FMT_TXT_CMAP_SPARSE_FULL,
    LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY,
    LV_FONT_FMT_TXT_CMAP_SPARSE_TINY,
};

typedef struct lv_font_t lv_font_t;

typedef struct {
    uint32_t bitmap_index;
    uint32_t adv_w;            // 1/16 px
    uint16_t box_w, box_h;
    int16_t  ofs_x, ofs_y;
} lv_font_fmt_txt_glyph_dsc_t;

typedef struct {
    uint32_t range_start;
    uint16_t range_length;
    uint16_t glyph_id_start;
    const uint16_t* unicode_list;
    const void* glyph_id_ofs_list;
    uint16_t list_length;
    int type;
} lv_font_fmt_txt_cmap_t;

typedef struct {
    const void* glyph_ids;     // uint8_t pairs (glyph_ids_size 0) or uint16_t pairs
    const int8_t* values;
    uint32_t pair_cnt;
    uint32_t glyph_ids_size;
} lv_font_fmt_txt_kern_pair_t;

typedef struct {
    const uint8_t* glyph_bitmap;
    const lv_font_fmt_txt_glyph_dsc_t* glyph_dsc;
    const lv_font_fmt_txt_cmap_t* cmaps;
    const void* kern_dsc;
    uint16_t kern_scale;
    uint16_t cmap_num;
    uint16_t bpp;
    uint16_t kern_classes;
    uint16_t bitmap_format;
} lv_font_fmt_txt_dsc_t;

bool lv_font_get_glyph_dsc_fmt_txt(const lv_font_t* font, void* dsc_out, uint32_t letter, uint32_t letter_next);
const void* lv_font_get_bitmap_fmt_txt(void* glyph_dsc, void* draw_buf);

struct lv_font_t {
    bool (*get_glyph_dsc)(const lv_font_t*, void*, uint32_t, uint32_t);
    const void* (*get_glyph_bitmap)(void*, void*);
    void (*release_glyph)(const lv_font_t*, void*);
    int32_t line_height;
    int32_t base_line;
    uint8_t subpx;
    uint8_t kerning;
    uint8_t static_bitmap;
    int8_t  underline_position;
    int8_t  underline_thickness;
    const void* dsc;
    const lv_font_t* fallback;
    void* user_data;
};

uint16_t lv_font_get_glyph_width(const lv_font_t* font, uint32_t letter, uint32_t letter_next);
int32_t  lv_font_get_line_height(const lv_font_t* font);

#ifdef __cplusplus
}
#endif
