#pragma once
#include <lvgl.h>
#include "theme_palette.h"

// Design tokens — single source of truth for UI colors. The defaults are in
// theme_palette.cpp; the app can change any of them at run time (payload
// `th`, kept in NVS by clawd_prefs.cpp), so read them when drawing and
// register long-lived styles in ui.cpp so a new palette repaints them.
static inline lv_color_t theme_color(ThemeColor c) { return lv_color_hex(theme_hex(c)); }

#define THEME_BG       theme_color(TH_BG)       // screen background
#define THEME_PANEL    theme_color(TH_PANEL)    // card/zone fill
#define THEME_TEXT     theme_color(TH_TEXT)     // primary text
#define THEME_DIM      theme_color(TH_DIM)      // secondary text
#define THEME_ACCENT   theme_color(TH_ACCENT)   // brand terra-cotta by default
#define THEME_GREEN    theme_color(TH_GREEN)
#define THEME_AMBER    theme_color(TH_AMBER)
#define THEME_RED      theme_color(TH_RED)
#define THEME_BAR_BG   theme_color(TH_BAR_BG)   // unfilled bar track
