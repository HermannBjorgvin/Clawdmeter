#pragma once
#include <stdint.h>

// The UI's colour tokens as 0xRRGGBB, changeable at run time: the app sends
// the whole palette as `th`, eleven 6-digit hex colours run together in this
// order (no '#', no separators). Pure: no LVGL, no Arduino, so the parser
// runs in the native tests. theme.h wraps these as lv_color_t.
enum ThemeColor : uint8_t {
    TH_BG,       // screen background
    TH_PANEL,    // card/zone fill
    TH_TEXT,     // primary text
    TH_DIM,      // secondary text
    TH_ACCENT,   // status line, pairing notes
    TH_GREEN,    // bars: under 50 % / on pace
    TH_AMBER,    // bars: 50-80 % / ahead of pace
    TH_RED,      // bars: 80 %+ / at the limit
    TH_BAR_BG,   // unfilled bar track, pills
    TH_FACE,     // the screensaver face's body
    TH_EYE,      // the screensaver face's eyes
    TH_COUNT
};

#define THEME_HEX_LEN (TH_COUNT * 6)

extern const uint32_t THEME_DEFAULTS[TH_COUNT];

// `s` must be exactly THEME_HEX_LEN hex digits (either case); anything else
// returns false and leaves `out` untouched.
bool theme_parse_hex(const char* s, uint32_t out[TH_COUNT]);
void theme_format_hex(const uint32_t in[TH_COUNT], char out[THEME_HEX_LEN + 1]);

// The live palette (starts at THEME_DEFAULTS).
uint32_t theme_hex(ThemeColor c);
// Replace the live palette; true when any colour changed.
bool theme_palette_set(const uint32_t p[TH_COUNT]);
