#pragma once
#include <lvgl.h>
#include <stdint.h>
#include <stdbool.h>

// One on-screen pet: an LVGL image whose pixels are the logical pet canvas
// (pet_buddy.h) scaled up by a whole factor. Two exist, never visible
// together: the small one in the meters' seat (pet_mascot.cpp) and the big
// one on the overlay (pet_face.cpp).

struct PetView {
    lv_obj_t*      img;
    uint8_t*       buf;
    lv_image_dsc_t dsc;
    int            scale;
    bool           alpha;    // true: background transparent (RGB565A8); false: opaque black
};

// Allocates the pixel buffer (external RAM first, then internal) and creates
// the image, hidden. False when there is no memory for it.
bool pet_view_create(PetView* v, lv_obj_t* parent, int scale, bool alpha);
int  pet_view_w(const PetView* v);
int  pet_view_h(const PetView* v);
// Render the current frame of `state` and redraw the image.
void pet_view_draw(PetView* v, uint8_t state);
void pet_view_show(PetView* v, bool on);
