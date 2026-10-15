// LVGL's two font calls the layout uses, over the real font data
// (fonts.c): glyph advance with kerning, rounded per glyph as
// lv_font_get_glyph_dsc_fmt_txt does, and the line height.
#include "lvgl.h"
#include <stddef.h>

bool lv_font_get_glyph_dsc_fmt_txt(const lv_font_t* f, void* d, uint32_t l, uint32_t n) {
    (void)f; (void)d; (void)l; (void)n;
    return false;
}
const void* lv_font_get_bitmap_fmt_txt(void* g, void* b) { (void)g; (void)b; return NULL; }

static uint32_t glyph_id(const lv_font_fmt_txt_dsc_t* d, uint32_t cp) {
    if (!cp) return 0;
    for (uint16_t i = 0; i < d->cmap_num; i++) {
        const lv_font_fmt_txt_cmap_t* c = &d->cmaps[i];
        const uint32_t rcp = cp - c->range_start;
        if (rcp >= c->range_length) continue;
        if (c->type == LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY) return c->glyph_id_start + rcp;
        if (c->type == LV_FONT_FMT_TXT_CMAP_SPARSE_TINY) {
            for (uint16_t k = 0; k < c->list_length; k++)
                if (c->unicode_list[k] == rcp) return c->glyph_id_start + k;
            return 0;
        }
        return 0;   // the FULL formats aren't used by these fonts
    }
    return 0;
}

static int8_t kern(const lv_font_fmt_txt_dsc_t* d, uint32_t left, uint32_t right) {
    const lv_font_fmt_txt_kern_pair_t* k = (const lv_font_fmt_txt_kern_pair_t*)d->kern_dsc;
    if (!k || d->kern_classes || k->glyph_ids_size != 0) return 0;
    const uint8_t* ids = (const uint8_t*)k->glyph_ids;
    for (uint32_t i = 0; i < k->pair_cnt; i++)
        if (ids[2 * i] == left && ids[2 * i + 1] == right) return k->values[i];
    return 0;
}

uint16_t lv_font_get_glyph_width(const lv_font_t* font, uint32_t letter, uint32_t next) {
    if (letter < 0x20) return 0;
    const lv_font_fmt_txt_dsc_t* d = (const lv_font_fmt_txt_dsc_t*)font->dsc;
    const uint32_t gid = glyph_id(d, letter);
    if (!gid) return 0;
    const uint32_t gnext = font->kerning == 0 ? glyph_id(d, next) : 0;
    const int32_t kv = gnext ? ((int32_t)kern(d, gid, gnext) * d->kern_scale) >> 4 : 0;
    int32_t adv = (int32_t)d->glyph_dsc[gid].adv_w + kv;
    if (adv < 0) adv = 0;
    return (uint16_t)((adv + 8) >> 4);
}

int32_t lv_font_get_line_height(const lv_font_t* font) { return font->line_height; }
