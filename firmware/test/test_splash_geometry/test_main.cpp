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

int main() {
    UNITY_BEGIN();
    RUN_TEST(should_render_full_size_with_no_scaling_when_psram_present);
    RUN_TEST(should_render_tiny_buffer_and_scale_up_when_no_psram);
    RUN_TEST(should_size_from_smaller_dimension_on_portrait_panel);
    RUN_TEST(should_clamp_cell_to_at_least_one_on_tiny_panel);
    return UNITY_END();
}
