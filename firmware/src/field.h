#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <lvgl.h>

// A flat-colour shape on an overlay layer that redraws only what changes: a
// body rectangle plus a couple of small extra rectangles (the face's arms).
//
// The face and the orange streak are big solid rectangles that slide across
// the screen. As plain LVGL objects, every move invalidates their whole old
// and new area (for the rising face that's most of the screen, every frame),
// and each frame becomes a near full-screen render + panel flush. A field is
// instead a full-screen object that never moves: it draws its rectangle in
// LV_EVENT_DRAW_MAIN, and moving the rectangle invalidates only the strips
// that changed colour (old XOR new). Children (the face's eyes) still work
// like any LVGL child.
//
// Resting state is LV_OBJ_FLAG_HIDDEN with an empty rectangle. Showing,
// hiding or restacking the full-screen object would invalidate the whole
// screen even though it draws nothing at that moment, so those happen with
// LVGL's invalidation paused (field_show / field_hide / field_to_front all
// require an empty rectangle and no visible children).

#define FIELD_EXTRAS 2

struct FieldShape {
    lv_area_t body;                  // screen coords; empty when x2 < x1 or y2 < y1
    lv_area_t extra[FIELD_EXTRAS];   // empty when x2 < x1
};

struct Field {
    lv_obj_t*  obj;
    FieldShape shape;
    lv_color_t color;
};

void field_init(Field* f, lv_obj_t* parent, int32_t w, int32_t h, lv_color_t color);
void field_set(Field* f, int32_t x1, int32_t y1, int32_t x2, int32_t y2);   // a flat rectangle
// Any shape. Invalidates only what changed: old vs new of the body and of
// each extra (field_invalidate_change).
void field_set_shape(Field* f, const FieldShape* s);
// Invalidate, on `obj`, every pixel that differs between a solid rectangle
// at `o` and at `n` (either may be empty): old XOR new, in at most four
// areas. For solid children moved with invalidation paused (field_quietly),
// like the face's open eyes.
void field_invalidate_change(lv_obj_t* obj, const lv_area_t* o, const lv_area_t* n);
void field_shape_init(FieldShape* s);      // empty body, no extras
void field_clear(Field* f);                 // draw nothing (invalidates what it drew)

void field_show(Field* f);                  // un-hide, no redraw (rect must be empty)
void field_hide(Field* f);                  // hide, no redraw (clears the rect first)
void field_to_front(Field* f);              // top of its layer, no redraw (rect must be empty)

// Run `fn` with the display's invalidation paused: for a change whose pixels
// the caller has already invalidated (or that can't change any pixel).
// Layout already pending elsewhere is realised first, with its redraw, so
// only `fn`'s own invalidation is dropped.
void field_quietly(lv_obj_t* any, void (*fn)(void* ctx), void* ctx);
