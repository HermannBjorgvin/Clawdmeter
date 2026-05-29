// Design tokens for the 3.49" Clawdmeter UI.
// Mirrors firmware/src/theme.h on the 1.8" build so both screens share the
// same Anthropic-inspired palette. If/when the 1.8" theme changes, keep
// these in sync.

#pragma once
#include "lvgl.h"

#define COL_BG       lv_color_hex(0x000000)   // screen background (true black)
#define COL_PANEL    lv_color_hex(0x1f1f1e)   // card fill
#define COL_TEXT     lv_color_hex(0xfaf9f5)   // primary text (cream)
#define COL_DIM      lv_color_hex(0xb0aea5)   // secondary text
#define COL_ACCENT   lv_color_hex(0xd97757)   // brand terra-cotta
#define COL_GREEN    lv_color_hex(0x788c5d)
#define COL_AMBER    lv_color_hex(0xd97757)
#define COL_RED      lv_color_hex(0xc0392b)
#define COL_BAR_BG   lv_color_hex(0x2a2a28)   // unfilled bar track
