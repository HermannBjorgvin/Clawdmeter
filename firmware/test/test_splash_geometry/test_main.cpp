// Host unit test for splash_compute_geometry — the pure sizing decision behind
// the splash canvas. No Arduino/LVGL/hardware deps:
//
//   pio test -d firmware -e native -f test_splash_geometry

#include "splash_geometry.h"
#include <unity.h>

#define CHECK(cond) TEST_ASSERT_TRUE(cond)

void setUp(void) {}
void tearDown(void) {}

static void should_render_full_size_with_no_scaling_when_psram_present() {
    SplashGeometry g = splash_compute_geometry(480, 480, /*has_psram=*/true);
    CHECK(g.cell == 8);             // 480 / 60
    CHECK(g.canvas_dim == 480);     // full-size buffer in PSRAM
    CHECK(g.scale == 256);          // 1.0x — identical to legacy S3 behaviour
}

static void should_render_tiny_buffer_and_scale_up_when_no_psram() {
    SplashGeometry g = splash_compute_geometry(480, 480, /*has_psram=*/false);
    CHECK(g.cell == 1);             // 60x60 buffer => 7.2 KB of internal SRAM
    CHECK(g.canvas_dim == 60);
    CHECK(g.scale == 256 * 8);      // upscale to fill the panel
    // On-screen size must match the PSRAM path: 60 * 8 == 480.
    CHECK(g.canvas_dim * g.scale / 256 == 480);
}

static void should_size_from_smaller_dimension_on_portrait_panel() {
    // Portrait panel (e.g. AMOLED-1.8 geometry) without PSRAM: square canvas
    // sized to the smaller dimension, centred with vertical margin.
    SplashGeometry g = splash_compute_geometry(368, 448, /*has_psram=*/false);
    CHECK(g.cell == 1);
    CHECK(g.scale == 256 * (368 / 60));   // 6x => 360px square, centred
}

static void should_clamp_cell_to_at_least_one_on_tiny_panel() {
    SplashGeometry g = splash_compute_geometry(10, 10, /*has_psram=*/true);
    CHECK(g.cell == 1);             // 10/60 floors to 0 -> clamped to 1
    CHECK(g.canvas_dim == 60);
}

static void rectangular_panel_is_visible_edge_to_edge() {
    int x0, x1;
    panel_row_span(480, 480, /*round=*/false, 0, &x0, &x1);
    CHECK(x0 == 0 && x1 == 480);
    panel_row_span(368, 448, false, 447, &x0, &x1);
    CHECK(x0 == 0 && x1 == 368);
}

static void round_panel_is_visible_along_the_chord() {
    int x0, x1;
    panel_row_span(466, 466, /*round=*/true, 233, &x0, &x1);   // the middle: the whole width
    CHECK(x0 == 0 && x1 == 466);
    panel_row_span(466, 466, true, 407, &x0, &x1);             // his feet row: sqrt(233^2-174^2) = 154
    CHECK(x0 == 233 - 154 && x1 == 233 + 154);
    panel_row_span(466, 466, true, 0, &x0, &x1);               // the very top: a point
    CHECK(x1 - x0 <= 2);
    // Symmetric about the vertical axis, and narrowing away from the middle.
    int a0, a1, b0, b1;
    panel_row_span(466, 466, true, 233 - 100, &a0, &a1);
    panel_row_span(466, 466, true, 233 + 100, &b0, &b1);
    CHECK(a0 == b0 && a1 == b1);
    CHECK(a0 > 0 && a1 < 466);
}

static void sprites_walk_on_and_off_at_the_circle_not_the_bounding_box() {
    // Rectangular panel: just past the panel's own edges, as ever.
    CHECK(offstage_left_x(480, 480, false, 400, 200) == -200);
    CHECK(offstage_right_x(480, 480, false, 400) == 480);
    // Round panel, feet row 407: he is off the glass at the circle's edge on
    // that row (x 79 .. 387), well inside the bounding box. A sprite 300 px
    // wide ends at 387, and leaving left it must start at 79 - 300.
    CHECK(offstage_left_x(466, 466, true, 407, 300) == 79 - 300);
    CHECK(offstage_right_x(466, 466, true, 407) == 387);
    CHECK(offstage_left_x(466, 466, true, 407, 300) > -300);
    // At the middle row the circle reaches the bounding box: no difference.
    CHECK(offstage_left_x(466, 466, true, 233, 300) == -300);
    CHECK(offstage_right_x(466, 466, true, 233) == 466);
}

static void span_in_grid_cells() {
    int c0, c1;
    // 60-cell grid of 7 px cells centred on a 466 px panel (starts at 23).
    span_to_cells(23, 7, 60, 79, 387, &c0, &c1);
    CHECK(c0 == 8 && c1 == 52);               // cells 8..51 lie fully inside [79, 387)
    span_to_cells(23, 7, 60, 0, 466, &c0, &c1);   // a rectangular panel: the whole grid
    CHECK(c0 == 0 && c1 == 60);
    span_to_cells(23, 7, 60, 300, 100, &c0, &c1); // empty span: no cells
    CHECK(c1 == c0);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(should_render_full_size_with_no_scaling_when_psram_present);
    RUN_TEST(should_render_tiny_buffer_and_scale_up_when_no_psram);
    RUN_TEST(should_size_from_smaller_dimension_on_portrait_panel);
    RUN_TEST(should_clamp_cell_to_at_least_one_on_tiny_panel);
    RUN_TEST(rectangular_panel_is_visible_edge_to_edge);
    RUN_TEST(round_panel_is_visible_along_the_chord);
    RUN_TEST(sprites_walk_on_and_off_at_the_circle_not_the_bounding_box);
    RUN_TEST(span_in_grid_cells);
    return UNITY_END();
}
