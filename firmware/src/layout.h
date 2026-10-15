#pragma once
#include <stdint.h>
#include <lvgl.h>

// Where everything on the meters goes, worked out from the panel's shape
// alone. No LVGL objects here, only font metrics, so test_layout runs it for
// every supported panel size on the host.
//
// Three tuned designs (large 480x480, compact 368x448, small 240x240) set
// the type and spacing. A panel takes the largest design that fits it, then
// a few rules adapt it: narrow panels shorten labels and tighten spacing,
// tall ones share out their spare height, a short one squeezes, and a round
// one keeps inside its circle. See docs/porting/adding-a-board.md.

// The panel: its pixel size, and whether the glass is a circle inscribed in
// that rectangle (BoardCaps.round_panel).
struct LayoutShape {
    int16_t w, h;
    bool    round;
};

struct LayoutRect { int16_t x, y, w, h; };

struct Layout {
    int16_t w, h;

    // Header: the corner Clawd's slot on the left, the battery on the
    // right, the clock (or "Usage") between them.
    bool    small_icons;             // 40 px Clawd slot + 24 px battery (else 80 / 48)
    int16_t logo_x, logo_y;          // Clawd slot, top-left
    int16_t batt_x, batt_y;          // battery icon, top-left
    int16_t title_cx;                // title centre x
    // Per clock format, the largest title font that fits between the logo
    // and the battery, and its top edge (a smaller font sits on the same
    // centre line): [0] "Usage" and the 24-hour clock, [1] the 12-hour
    // clock. NULL: no room, the title stays hidden.
    const lv_font_t* title_font[2];
    int16_t title_y[2];

    // The two usage cards, stacked.
    LayoutRect card[2];
    int16_t pad_x, pad_y;            // card padding
    int16_t bar_y, bar_h;            // bar top, inside the padding
    int16_t reset_y;                 // reset line top, inside the padding
    int16_t pill_pad_x, pill_pad_y;
    const lv_font_t* pct_font;       // big percentage
    const lv_font_t* ent_pct_font;   // spend-capped account's spending number
    const lv_font_t* pill_font;
    const lv_font_t* reset_font;     // "Resets in ..." line
    const lv_font_t* period_font;    // spend-capped period line (reset_font when it fits)
    bool    period_short;            // no room for its pace word: "Resets <date>" in the pace colour
    const lv_font_t* pace_font;
    const char* const* pills;        // PILL_TEXT or PILL_TEXT_SHORT (ui_text.h)

    // Status line, centred at the bottom.
    const lv_font_t* status_font;
    int16_t status_y;                // bottom edge offset from the panel's bottom (<= 0)
    int16_t status_w;                // widest it may draw; longer verbs are skipped

    // Pairing hint: four centred lines (heading, two steps, note), each
    // line_y below pair_y. Where a step is wider than pair_w the lines wrap
    // there (pair_wrap) and the lines under them sit lower to make room.
    int16_t pair_y, pair_w;
    bool    pair_wrap;
    const lv_font_t* pair_font[4];
    int16_t pair_line_y[4];
};

Layout layout_compute(const LayoutShape& shape);

// Corner icon sizes the layout plans with (ui.cpp checks them against the art).
#define LAYOUT_LOGO_SLOT        80   // logo.h LOGO_HEIGHT
#define LAYOUT_LOGO_SLOT_SMALL  40
#define LAYOUT_LOGO_W           72   // clawd_still.h CLAWD_STILL_W
#define LAYOUT_LOGO_W_SMALL     48
#define LAYOUT_BATT_W           48   // icons.h ICON_BATTERY_W
#define LAYOUT_BATT_W_SMALL     24

// Pixel width of UTF-8 `text` in `font`, as LVGL draws it (kerning included).
int32_t layout_text_width(const lv_font_t* font, const char* text);
