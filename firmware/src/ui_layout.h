#pragma once

// Per-board layout constants for ui.cpp. The 2.16" board (480x480) and
// the 1.8" board (368x448) have different aspect ratios so absolute
// positions can't simply scale — give each its own coordinate set.
//
// Origin is top-left of the panel. Y grows downward. All values are
// in raw pixels.

#include "display_cfg.h"

#ifdef BOARD_AMOLED_18
    // ESP32-S3-Touch-AMOLED-1.8 (368x448)
    #define UI_SCR_W            368
    #define UI_SCR_H            448
    #define UI_MARGIN           12
    #define UI_TITLE_Y          24
    #define UI_CONTENT_Y        88

    // Usage screen panels
    #define UI_PANEL_H          138
    #define UI_PANEL_GAP        12
    #define UI_BAR_Y_IN_PANEL   52
    #define UI_BAR_H            22
    #define UI_RESET_Y_IN_PANEL 88

    // Top overlay (logo + battery)
    #define UI_BATTERY_RIGHT    48      // icon width
    #define UI_LOGO_Y_OFFSET    (-10)

    // Bottom anim label
    #define UI_ANIM_OFFSET_Y    (-12)

    // Splash canvas — 20x20 grid, smaller cells than 2.16
    #define UI_SPLASH_GRID      20
    #define UI_SPLASH_CELL      18      // 20*18 = 360, fits in 368x448 centered
#else
    // ESP32-S3-Touch-AMOLED-2.16 (480x480) — upstream layout
    #define UI_SCR_W            480
    #define UI_SCR_H            480
    #define UI_MARGIN           20
    #define UI_TITLE_Y          30
    #define UI_CONTENT_Y        100

    #define UI_PANEL_H          150
    #define UI_PANEL_GAP        16
    #define UI_BAR_Y_IN_PANEL   56
    #define UI_BAR_H            24
    #define UI_RESET_Y_IN_PANEL 94

    #define UI_BATTERY_RIGHT    48
    #define UI_LOGO_Y_OFFSET    (-10)

    #define UI_ANIM_OFFSET_Y    (-15)

    #define UI_SPLASH_GRID      20
    #define UI_SPLASH_CELL      24      // 20*24 = 480
#endif

#define UI_CONTENT_W   (UI_SCR_W - 2 * UI_MARGIN)
