#include "streak.h"
#include "theme.h"
#include "perf.h"
#include "field.h"
#include "hal/board_caps.h"

#define STREAK_MS 380

static Field     band;                  // the band: columns [band_x, band_x + band_w)
static lv_obj_t* cover_obj = NULL;
static int W = 480, H = 480, band_w = 150;
static int band_x = 480;
static bool running = false;

void streak_init(void) {
    const BoardCaps& c = board_caps();
    W = c.width; H = c.height;
    const int mind = W < H ? W : H;
    band_w = 150 * mind / 480;
    field_init(&band, lv_layer_top(), W, H, THEME_ACCENT);
}

static void set_cover_width(void* ctx) {
    lv_obj_set_width(cover_obj, (int32_t)(intptr_t)ctx);
}

static void x_exec(void* var, int32_t x) {
    (void)var;
    const int old_x = band_x;
    band_x = x;
    // The band redraws only the columns it gained and lost.
    field_set(&band, x, 0, x + band_w - 1, H - 1);
    // Stage stays left of the band's middle; the meters are revealed right
    // of it. The band covers the seam. Every pixel the narrower stage gives
    // up is either still under the band or in a column the band just
    // left (invalidated above), so the resize itself needs no redraw: done
    // quietly, else LVGL would repaint the whole stage every frame.
    if (cover_obj && x != old_x) {
        int w = x + band_w / 2;
        if (w < 0) w = 0;
        if (w > W) w = W;
        if (w != lv_obj_get_width(cover_obj))
            field_quietly(cover_obj, set_cover_width, (void*)(intptr_t)w);
    }
}

static void done(lv_anim_t* a) {
    (void)a;
    running = false;
    perf_end(PERF_STREAK);
    field_hide(&band);
}

void streak_run(lv_obj_t* cover) {
    if (!band.obj) return;
    cover_obj = cover;
    lv_anim_delete(&band, x_exec);
    // Starts just off the right edge: nothing on screen yet, so it shows
    // and goes on top of the face without a redraw.
    field_clear(&band);
    field_show(&band);
    field_to_front(&band);
    band_x = W;
    running = true;
    perf_begin(PERF_STREAK);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &band);
    lv_anim_set_exec_cb(&a, x_exec);
    lv_anim_set_values(&a, W, -band_w);
    lv_anim_set_duration(&a, STREAK_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}

bool streak_running(void) { return running; }

void streak_cancel(void) {
    if (!band.obj) return;
    lv_anim_delete(&band, x_exec);
    if (running) perf_end(PERF_STREAK);
    running = false;
    field_hide(&band);                    // repaints where the band was
    if (cover_obj) lv_obj_set_width(cover_obj, W);
    cover_obj = NULL;
}
