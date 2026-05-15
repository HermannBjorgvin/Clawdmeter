#pragma once
//
// Per-board UI layout tokens.
//
// All sizing/spacing/font choices for the dashboard live here so ui.cpp
// can stay board-agnostic. When porting to a new display, add a
// BOARD_<NAME> block below — pick fonts that fit, decide whether the
// logo and battery icon make sense at this scale, and you're done.
//

#include "display_cfg.h"
#include <lvgl.h>

LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_styrene_12);
LV_FONT_DECLARE(font_mono_32);
LV_FONT_DECLARE(font_mono_18);

#if defined(BOARD_WAVESHARE_AMOLED_216)

// Original 480x480 layout, tuned for the round-square AMOLED's rounded
// corners (wider 20 px margins).
#define UI_MARGIN              20
#define UI_TITLE_Y             30
#define UI_CONTENT_Y           100
#define UI_PANEL_H             150
#define UI_PANEL_GAP           16
#define UI_PANEL_PAD           16
#define UI_BAR_H               24
#define UI_BAR_Y               56
#define UI_RESET_Y             94

#define UI_BLE_PANEL_H         160
#define UI_BLE_RESET_H         110
#define UI_BLE_STATUS_X        56
#define UI_BLE_DEVICE_Y        64
#define UI_BLE_MAC_Y           100
#define UI_CREDIT_OFFSET       46
#define UI_CREDIT2_OFFSET      20

#define UI_FONT_TITLE          (&font_tiempos_56)
#define UI_FONT_PCT            (&font_styrene_48)
#define UI_FONT_BODY           (&font_styrene_28)
#define UI_FONT_DIM            (&font_styrene_28)
#define UI_FONT_PILL           (&font_styrene_28)
#define UI_FONT_ANIM           (&font_mono_32)
#define UI_FONT_CREDIT         (&font_styrene_24)
#define UI_FONT_CREDIT_SMALL   (&font_styrene_20)
#define UI_FONT_BLE_STATUS     (&font_styrene_48)

// 48x48 icons drawn at native size.
#define UI_ICON_W              48
#define UI_ICON_H              48
#define UI_SHOW_LOGO           1
#define UI_LOGO_X              UI_MARGIN
#define UI_LOGO_Y              (UI_TITLE_Y - 10)
#define UI_TITLE_X_OFFSET      16          // shift past the logo on the left

// Touch-only Bluetooth reset zone — gated by BOARD_HAS_TOUCH.

#elif defined(BOARD_LILYGO_T_DISPLAY_S3)

// 170x320 ST7789. No rounded corners, no touch — every interactive
// element has to be reachable via the two physical buttons, so the BLE
// "Reset Bluetooth" tap zone is omitted (BOARD_HAS_TOUCH is 0 and ui.cpp
// guards on that).
#define UI_MARGIN              8
#define UI_TITLE_Y             4
#define UI_CONTENT_Y           42
#define UI_PANEL_H             96
#define UI_PANEL_GAP           6
#define UI_PANEL_PAD           8
#define UI_BAR_H               12
#define UI_BAR_Y               36
#define UI_RESET_Y             60

#define UI_BLE_PANEL_H         170
#define UI_BLE_RESET_H         0          // not used; reset zone not built
#define UI_BLE_STATUS_X        32
#define UI_BLE_DEVICE_Y        40
#define UI_BLE_MAC_Y           96
// Credits wrap to two lines each on this width (12px font) — leave room
// for ~26 px per label plus an 8 px gap between them.
#define UI_CREDIT_OFFSET       40
#define UI_CREDIT2_OFFSET      8

#define UI_FONT_TITLE          (&font_styrene_24)
#define UI_FONT_PCT            (&font_styrene_28)
#define UI_FONT_BODY           (&font_styrene_14)
#define UI_FONT_DIM            (&font_styrene_12)
#define UI_FONT_PILL           (&font_styrene_12)
#define UI_FONT_ANIM           (&font_mono_18)
#define UI_FONT_CREDIT         (&font_styrene_12)
#define UI_FONT_CREDIT_SMALL   (&font_styrene_12)
#define UI_FONT_BLE_STATUS     (&font_styrene_16)

// 48x48 icons would dwarf this panel — draw them at 24x24 by setting
// the widget size explicitly and letting LVGL stretch the source.
#define UI_ICON_W              24
#define UI_ICON_H              24
#define UI_SHOW_LOGO           0          // 80x80 logo doesn't fit
#define UI_LOGO_X              0
#define UI_LOGO_Y              0
#define UI_TITLE_X_OFFSET      -8         // nudge left so the title clears the battery icon

#endif

// Common derived constant.
#define UI_CONTENT_W           (BOARD_LCD_W - 2 * UI_MARGIN)
