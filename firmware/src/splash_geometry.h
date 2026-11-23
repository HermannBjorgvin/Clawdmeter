#pragma once
#include <stdint.h>

// Pure geometry for the grid Clawd performs on at full size: 60 rows, and 60
// columns plus a margin that widens it to the panel (performer.cpp; mascot.cpp
// sizes the lurk and attention sprites from the 60-cell size).
// Deliberately free of LVGL/Arduino dependencies so it can be unit-tested on
// the host (see test/test_splash_geometry/). Decides the in-buffer pixel size
// of each grid cell and the LVGL image scale needed to fill the panel.
//
// With PSRAM the buffer is rendered at full panel size (scale = 1.0x).
// Without it, the geometry is a tiny 60x60 buffer (~7 KB) for LVGL to upscale
// with nearest-neighbour; the performer doesn't use that path (it needs
// per-pixel alpha at full size), so PSRAM-less boards play corner acts.

#define SPLASH_GRID         60
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

// Whole cells to add on each side of the 60-column grid so that a canvas of
// cell-px cells spans a panel width px wide: 0 on a square or portrait panel,
// 10 on 320x240 (4 px cells). The performer draws on this wider grid so a
// walk reaches the real edge of a landscape panel, not the square's.
static inline int splash_side_cells(int width, int cell) {
    const int spare = width - SPLASH_GRID * cell;
    if (spare <= 0 || cell < 1) return 0;
    return (spare / 2 + cell - 1) / cell;
}

// ─── Round panels ────────────────────────────────────────────────────────────
// On a round panel the glass is the circle inscribed in width x height, so a
// full-size Clawd that walks on or off along a row enters and leaves at the
// circle's edge on that row, not at the bounding box's. Both helpers are pure
// so the host can test them.

// The visible horizontal span [*x0, *x1) of panel row y: the whole width on a
// rectangular panel, the chord of the inscribed circle on a round one (empty
// outside it).
static inline void panel_row_span(int width, int height, bool round, int y,
                                  int* x0, int* x1) {
    if (!round) { *x0 = 0; *x1 = width; return; }
    const int r = (width < height ? width : height) / 2;
    const int dy = y - height / 2;
    long rem = (long)r * r - (long)dy * dy;
    if (rem < 0) rem = 0;
    int half = 0;
    while ((long)(half + 1) * (half + 1) <= rem) half++;   // floor(sqrt(rem))
    *x0 = width / 2 - half;
    *x1 = width / 2 + half;
}

// Where a sprite sw px wide stands once it has walked off the visible span of
// row y: all of it past the left edge / past the right edge.
static inline int offstage_left_x(int width, int height, bool round, int y, int sw) {
    int x0, x1;
    panel_row_span(width, height, round, y, &x0, &x1);
    return x0 - sw;
}
static inline int offstage_right_x(int width, int height, bool round, int y) {
    int x0, x1;
    panel_row_span(width, height, round, y, &x0, &x1);
    return x1;
}

// That span as whole cells of a grid whose cell 0 starts at pixel grid_x:
// the cells fully inside it, clamped to [0, grid_cells].
static inline void span_to_cells(int grid_x, int cell, int grid_cells,
                                 int x0, int x1, int* c0, int* c1) {
    int a = (x0 - grid_x + cell - 1) / cell;
    int b = (x1 - grid_x) / cell;
    if (x0 - grid_x < 0) a = 0;
    if (x1 - grid_x < 0) b = 0;
    if (a < 0) a = 0;
    if (b > grid_cells) b = grid_cells;
    if (b < a) b = a;
    *c0 = a;
    *c1 = b;
}

// The same span as the cells that show any of it, [*c0, *c1): a sprite is off
// the glass only once it is past these, partly visible cells included.
static inline void span_to_touched_cells(int grid_x, int cell, int grid_cells,
                                         int x0, int x1, int* c0, int* c1) {
    int a = x0 - grid_x;
    int b = x1 - grid_x;
    a = a <= 0 ? 0 : a / cell;
    b = b <= 0 ? 0 : (b + cell - 1) / cell;
    if (b > grid_cells) b = grid_cells;
    if (a > b) a = b;
    *c0 = a;
    *c1 = b;
}
