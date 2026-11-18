#ifdef PET_ASCII
#include "pet_view.h"
#include "pet_buddy.h"
#include <esp_heap_caps.h>
#include <string.h>

int pet_view_w(const PetView* v) { return PET_W * v->scale; }
int pet_view_h(const PetView* v) { return PET_H * v->scale; }

bool pet_view_create(PetView* v, lv_obj_t* parent, int scale, bool alpha) {
    memset(v, 0, sizeof(*v));
    v->scale = scale < 1 ? 1 : scale;
    v->alpha = alpha;
    const size_t px = (size_t)pet_view_w(v) * pet_view_h(v);
    const size_t bytes = px * (alpha ? 3 : 2);
    v->buf = (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!v->buf) v->buf = (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!v->buf) return false;
    memset(v->buf, 0, bytes);

    v->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    v->dsc.header.w = pet_view_w(v);
    v->dsc.header.h = pet_view_h(v);
    v->dsc.header.cf = alpha ? LV_COLOR_FORMAT_RGB565A8 : LV_COLOR_FORMAT_RGB565;
    v->dsc.header.stride = pet_view_w(v) * 2;
    v->dsc.data = v->buf;
    v->dsc.data_size = bytes;

    v->img = lv_image_create(parent);
    lv_obj_clear_flag(v->img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(v->img, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(v->img, &v->dsc);
    return true;
}

void pet_view_draw(PetView* v, uint8_t state) {
    if (!v->img || !v->buf) return;
    const uint16_t* src = pet_render(state);
    const int s = v->scale, w = pet_view_w(v), h = pet_view_h(v);
    uint16_t* color = (uint16_t*)v->buf;
    uint8_t*  alpha = v->alpha ? v->buf + (size_t)w * h * 2 : NULL;
    for (int y = 0; y < PET_H; y++) {
        // Scale one source row across, then copy it down s - 1 times.
        uint16_t* row = &color[(size_t)y * s * w];
        for (int x = 0; x < PET_W; x++) {
            const uint16_t c = src[y * PET_W + x];
            for (int dx = 0; dx < s; dx++) row[x * s + dx] = c;
        }
        for (int dy = 1; dy < s; dy++) memcpy(row + (size_t)dy * w, row, (size_t)w * 2);
        if (alpha) {
            uint8_t* arow = &alpha[(size_t)y * s * w];
            for (int x = 0; x < PET_W; x++)
                memset(arow + x * s, src[y * PET_W + x] ? 255 : 0, s);
            for (int dy = 1; dy < s; dy++) memcpy(arow + (size_t)dy * w, arow, w);
        }
    }
    lv_image_set_src(v->img, &v->dsc);
    lv_obj_invalidate(v->img);
}

void pet_view_show(PetView* v, bool on) {
    if (!v->img) return;
    if (on) lv_obj_clear_flag(v->img, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(v->img, LV_OBJ_FLAG_HIDDEN);
}

#endif  // PET_ASCII
