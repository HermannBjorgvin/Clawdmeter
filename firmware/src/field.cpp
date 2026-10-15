#include "field.h"

static bool area_empty(const lv_area_t* a) { return a->x2 < a->x1 || a->y2 < a->y1; }

static bool intersect(lv_area_t* r, const lv_area_t* a, const lv_area_t* b) {
    r->x1 = a->x1 > b->x1 ? a->x1 : b->x1;
    r->y1 = a->y1 > b->y1 ? a->y1 : b->y1;
    r->x2 = a->x2 < b->x2 ? a->x2 : b->x2;
    r->y2 = a->y2 < b->y2 ? a->y2 : b->y2;
    return !area_empty(r);
}

void field_shape_init(FieldShape* s) {
    s->body.x1 = 0; s->body.y1 = 0; s->body.x2 = -1; s->body.y2 = -1;
    for (int i = 0; i < FIELD_EXTRAS; i++) s->extra[i] = s->body;
}

static void draw_cb(lv_event_t* e) {
    const Field* f = (const Field*)lv_event_get_user_data(e);
    const FieldShape* s = &f->shape;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = f->color;
    d.bg_opa = LV_OPA_COVER;
    d.radius = 0;
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!area_empty(&s->body)) lv_draw_rect(layer, &d, &s->body);
    for (int i = 0; i < FIELD_EXTRAS; i++)
        if (!area_empty(&s->extra[i])) lv_draw_rect(layer, &d, &s->extra[i]);
}

static void inv(lv_obj_t* obj, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    if (x2 < x1 || y2 < y1) return;
    lv_area_t a = { x1, y1, x2, y2 };
    lv_obj_invalidate_area(obj, &a);         // clipped to the object (the screen)
}

void field_invalidate_change(lv_obj_t* obj, const lv_area_t* o, const lv_area_t* n) {
    const bool oe = area_empty(o), ne = area_empty(n);
    if (oe && ne) return;
    if (!oe && !ne && o->x1 == n->x1 && o->y1 == n->y1 && o->x2 == n->x2 && o->y2 == n->y2) return;
    lv_area_t c;
    if (oe || ne || !intersect(&c, o, n)) {
        if (!oe) inv(obj, o->x1, o->y1, o->x2, o->y2);
        if (!ne) inv(obj, n->x1, n->y1, n->x2, n->y2);
        return;
    }
    // Rows only one of them spans (across both), then, in the shared rows,
    // the columns only one of them spans. A superset of old XOR new (it only
    // overdraws a corner when the rectangle both moves and resizes), in at
    // most four areas, so a few of these never overflow LVGL's 32-entry
    // invalidation buffer into a full-screen redraw.
    const int32_t ux1 = LV_MIN(o->x1, n->x1), ux2 = LV_MAX(o->x2, n->x2);
    inv(obj, ux1, LV_MIN(o->y1, n->y1), ux2, LV_MAX(o->y1, n->y1) - 1);
    inv(obj, ux1, LV_MIN(o->y2, n->y2) + 1, ux2, LV_MAX(o->y2, n->y2));
    inv(obj, LV_MIN(o->x1, n->x1), c.y1, LV_MAX(o->x1, n->x1) - 1, c.y2);
    inv(obj, LV_MIN(o->x2, n->x2) + 1, c.y1, LV_MAX(o->x2, n->x2), c.y2);
}

void field_init(Field* f, lv_obj_t* parent, int32_t w, int32_t h, lv_color_t color) {
    f->color = color;
    field_shape_init(&f->shape);
    f->obj = lv_obj_create(parent);
    lv_obj_remove_style_all(f->obj);
    lv_obj_set_pos(f->obj, 0, 0);
    lv_obj_set_size(f->obj, w, h);
    lv_obj_clear_flag(f->obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(f->obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(f->obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(f->obj, draw_cb, LV_EVENT_DRAW_MAIN, f);
}

static bool same_area(const lv_area_t* a, const lv_area_t* b) {
    if (area_empty(a) && area_empty(b)) return true;
    return a->x1 == b->x1 && a->y1 == b->y1 && a->x2 == b->x2 && a->y2 == b->y2;
}

void field_set_shape(Field* f, const FieldShape* s) {
    const FieldShape o = f->shape;
    const FieldShape& n = *s;
    f->shape = n;
    if (!same_area(&o.body, &n.body)) field_invalidate_change(f->obj, &o.body, &n.body);
    for (int i = 0; i < FIELD_EXTRAS; i++)
        if (!same_area(&o.extra[i], &n.extra[i])) field_invalidate_change(f->obj, &o.extra[i], &n.extra[i]);
}

void field_set(Field* f, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    FieldShape s;
    field_shape_init(&s);
    s.body.x1 = x1; s.body.y1 = y1; s.body.x2 = x2; s.body.y2 = y2;
    field_set_shape(f, &s);
}

void field_clear(Field* f) {
    FieldShape s;
    field_shape_init(&s);
    field_set_shape(f, &s);
}

void field_quietly(lv_obj_t* any, void (*fn)(void* ctx), void* ctx) {
    lv_obj_update_layout(any);               // others' pending changes keep their redraw
    lv_display_t* d = lv_obj_get_display(any);
    lv_display_enable_invalidation(d, false);
    fn(ctx);
    lv_obj_update_layout(any);               // realise fn's layout changes while paused
    lv_display_enable_invalidation(d, true);
}

static void do_show(void* o)  { lv_obj_clear_flag((lv_obj_t*)o, LV_OBJ_FLAG_HIDDEN); }
static void do_hide(void* o)  { lv_obj_add_flag((lv_obj_t*)o, LV_OBJ_FLAG_HIDDEN); }
static void do_front(void* o) { lv_obj_move_foreground((lv_obj_t*)o); }

void field_show(Field* f) {
    if (!lv_obj_has_flag(f->obj, LV_OBJ_FLAG_HIDDEN)) return;
    field_quietly(f->obj, do_show, f->obj);
}

void field_hide(Field* f) {
    field_clear(f);
    if (lv_obj_has_flag(f->obj, LV_OBJ_FLAG_HIDDEN)) return;
    field_quietly(f->obj, do_hide, f->obj);
}

void field_to_front(Field* f) {
    lv_obj_t* p = lv_obj_get_parent(f->obj);
    if (lv_obj_get_index(f->obj) == (int32_t)lv_obj_get_child_count(p) - 1) return;
    field_quietly(f->obj, do_front, f->obj);
}
