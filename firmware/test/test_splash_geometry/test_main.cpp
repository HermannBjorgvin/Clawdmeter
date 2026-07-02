// Host unit test for splash_compute_geometry — the pure sizing decision behind
// the splash canvas. No Arduino/LVGL/hardware deps, so it runs on any host:
//
//   g++ -std=c++17 -I ../../src test_main.cpp -o t && ./t
//
// (No compiler is bundled on the dev box used to author this; run it wherever
// a host g++/clang++ is available, or via a PlatformIO `native` env.)

#include "splash_geometry.h"
#include <cstdio>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);             \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static void should_render_full_size_with_no_scaling_when_psram_present() {
    SplashGeometry g = splash_compute_geometry(480, 480, /*has_psram=*/true);
    CHECK(g.cell == 24);            // 480 / 20
    CHECK(g.canvas_dim == 480);     // full-size buffer in PSRAM
    CHECK(g.scale == 256);          // 1.0x — identical to legacy S3 behaviour
}

static void should_render_tiny_buffer_and_scale_up_when_no_psram() {
    SplashGeometry g = splash_compute_geometry(480, 480, /*has_psram=*/false);
    CHECK(g.cell == 1);             // 20x20 buffer => 800 bytes of internal SRAM
    CHECK(g.canvas_dim == 20);
    CHECK(g.scale == 256 * 24);     // upscale to fill the panel
    // On-screen size must match the PSRAM path: 20 * 24 == 480.
    CHECK(g.canvas_dim * g.scale / 256 == 480);
}

static void should_size_from_smaller_dimension_on_portrait_panel() {
    // Portrait panel (e.g. AMOLED-1.8 geometry) without PSRAM: square canvas
    // sized to the smaller dimension, centred with vertical margin.
    SplashGeometry g = splash_compute_geometry(368, 448, /*has_psram=*/false);
    CHECK(g.cell == 1);
    CHECK(g.scale == 256 * (368 / 20));   // 18x => 360px square, centred
}

static void should_clamp_cell_to_at_least_one_on_tiny_panel() {
    SplashGeometry g = splash_compute_geometry(10, 10, /*has_psram=*/true);
    CHECK(g.cell == 1);             // 10/20 floors to 0 -> clamped to 1
    CHECK(g.canvas_dim == 20);
}

int main() {
    should_render_full_size_with_no_scaling_when_psram_present();
    should_render_tiny_buffer_and_scale_up_when_no_psram();
    should_size_from_smaller_dimension_on_portrait_panel();
    should_clamp_cell_to_at_least_one_on_tiny_panel();

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all splash geometry checks passed\n");
    return 0;
}
