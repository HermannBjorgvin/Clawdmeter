#pragma once

// Per-board UI layout metrics. Anything that depends on panel size, aspect
// ratio, or DPI lives here so ui.cpp / splash.cpp stay panel-agnostic.
//
// The shared values (margins, anchor y-offsets, panel heights) were tuned on
// the 480×480 Waveshare and look fine on the 450×600 LilyGO because every
// per-screen layout anchors to top or bottom — the extra vertical space on
// the portrait panel just falls in the middle. Per-board overrides are only
// added where the original metrics don't fit (e.g. the BT-screen MAC line on
// the 450 panel was clipped at the right margin with font_styrene_28).

#include "display_cfg.h"

LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_mono_32);

// ---- Screen ----
#define UI_SCR_W              LCD_WIDTH
#define UI_SCR_H              LCD_HEIGHT
#define UI_MARGIN             20    // inset from rounded display corners
#define UI_TITLE_Y            30
#define UI_CONTENT_Y          100
#define UI_CONTENT_W          (UI_SCR_W - 2 * UI_MARGIN)

// ---- Usage panels ----
#define UI_PANEL_H            150
#define UI_PANEL_GAP          16
#define UI_PANEL_BAR_Y        56
#define UI_PANEL_BAR_W        (UI_CONTENT_W - 32)
#define UI_PANEL_BAR_H        24
#define UI_PANEL_RESET_Y      94

// ---- Spinner / status line ----
#define UI_ANIM_MSG_MS        4000
#define UI_ANIM_BOTTOM_INSET  15   // y-offset from bottom for "Working…" line

// ---- Bluetooth screen ----
#define UI_BT_INFO_PANEL_H        160
#define UI_BT_RESET_ZONE_GAP      16
#define UI_BT_RESET_ZONE_H        110
#define UI_BT_CREDIT_BOTTOM       46
#define UI_BT_CREDIT2_BOTTOM      20

// Tightening the address font on the 450-wide panel keeps the MAC line
// inside the panel padding. The 28pt font fits on 480 but clips on 450.
#if defined(BOARD_LILYGO_T4S3)
#  define UI_BT_LINE_FONT     font_styrene_24
#else
#  define UI_BT_LINE_FONT     font_styrene_28
#endif

// ---- Battery / logo overlay (top bar) ----
#define UI_BATTERY_RIGHT_PAD  (48 + UI_MARGIN)   // distance from right edge
#define UI_BATTERY_Y          UI_TITLE_Y
#define UI_LOGO_X             UI_MARGIN
#define UI_LOGO_Y             (UI_TITLE_Y - 10)

// ---- Splash pixel-art canvas ----
// 20×20 source grid scaled to fill the panel's shorter dimension. Floor the
// integer division so the canvas stays square; lv_obj_center pushes it into
// the middle of any non-square panel.
#define UI_SPLASH_GRID        20
#define UI_SPLASH_CELL        (UI_SCR_W / UI_SPLASH_GRID)
#define UI_SPLASH_CANVAS_W    (UI_SPLASH_GRID * UI_SPLASH_CELL)
#define UI_SPLASH_CANVAS_H    (UI_SPLASH_GRID * UI_SPLASH_CELL)
