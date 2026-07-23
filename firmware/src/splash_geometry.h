#pragma once
#include <stdint.h>

// Pure geometry for the 20x20 splash canvas. Deliberately free of LVGL/Arduino
// dependencies so it can be unit-tested on the host (see
// test/test_splash_geometry/). Decides the in-buffer pixel size of each grid
// cell and the LVGL image scale needed to fill the panel.
//
// PSRAM boards render the canvas at full panel size (scale = 1.0x) as before.
// PSRAM-less boards (e.g. the ESP32-C6 sibling) cannot hold a 480x480 RGB565
// framebuffer (460 KB) in internal SRAM, so they render a tiny 20x20 buffer
// (~800 bytes) and let LVGL upscale it with nearest-neighbour. A full-screen
// splash then costs ~800 bytes instead of 460 KB, with no cropping.

#define SPLASH_GRID         20
#define SPLASH_SCALE_UNITY  256   // LVGL image-scale denominator (256 == 1.0x)

typedef struct {
    int cell;        // px per grid cell inside the canvas buffer
    int canvas_dim;  // SPLASH_GRID * cell (canvas is square)
    int scale;       // LVGL image scale in 1/256 units (SPLASH_SCALE_UNITY == none)
} SplashGeometry;

static inline SplashGeometry splash_compute_geometry(int width, int height,
                                                     bool has_psram) {
    int min_dim = (width < height) ? width : height;
    int target_cell = min_dim / SPLASH_GRID;   // desired on-screen px per cell
    if (target_cell < 1) target_cell = 1;

    SplashGeometry g;
    if (has_psram) {
        g.cell  = target_cell;             // render at full size...
        g.scale = SPLASH_SCALE_UNITY;      // ...and don't scale
    } else {
        g.cell  = 1;                       // tiny 20x20 buffer...
        g.scale = SPLASH_SCALE_UNITY * target_cell;  // ...scaled up to full size
    }
    g.canvas_dim = SPLASH_GRID * g.cell;
    return g;
}
