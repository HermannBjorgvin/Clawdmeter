#include "layout.h"
#include "ui_text.h"
#include <math.h>
#include <stdio.h>

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

// ─── Text metrics ────────────────────────────────────────────────────────────

static uint32_t next_codepoint(const char** p) {
    const uint8_t* s = (const uint8_t*)*p;
    uint32_t cp;
    int n;
    if (s[0] == 0)               return 0;
    if (s[0] < 0x80)             { cp = s[0];        n = 1; }
    else if ((s[0] >> 5) == 0x6) { cp = s[0] & 0x1F; n = 2; }
    else if ((s[0] >> 4) == 0xE) { cp = s[0] & 0x0F; n = 3; }
    else                         { cp = s[0] & 0x07; n = 4; }
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) { *p += i; return 0xFFFD; }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *p += n;
    return cp;
}

int32_t layout_text_width(const lv_font_t* font, const char* text) {
    int32_t w = 0;
    uint32_t cp = next_codepoint(&text);
    while (cp) {
        const uint32_t next = next_codepoint(&text);
        w += lv_font_get_glyph_width(font, cp, next);
        cp = next;
    }
    return w;
}

static int16_t line_h(const lv_font_t* f) { return (int16_t)lv_font_get_line_height(f); }

static int16_t widest(const lv_font_t* f, const char* const* texts, int n) {
    int32_t w = 0;
    for (int i = 0; i < n; i++) {
        const int32_t t = layout_text_width(f, texts[i]);
        if (t > w) w = t;
    }
    return (int16_t)w;
}

// Widest the clock can draw in one format: the widest hour part plus the
// widest minutes (the ':' between them doesn't kern with the digits).
static int16_t widest_clock(const lv_font_t* f, bool h12) {
    char buf[8];
    int32_t hw = 0, mw = 0;
    for (int h = h12 ? 1 : 0; h < (h12 ? 13 : 24); h++) {
        snprintf(buf, sizeof(buf), h12 ? "%d:" : "%02d:", h);
        const int32_t w = layout_text_width(f, buf);
        if (w > hw) hw = w;
    }
    for (int m = 0; m < 60; m++) {
        snprintf(buf, sizeof(buf), "%02d", m);
        const int32_t w = layout_text_width(f, buf);
        if (w > mw) mw = w;
    }
    int32_t w = hw + mw;
    if (h12) {
        const int32_t am = layout_text_width(f, " AM"), pm = layout_text_width(f, " PM");
        w += am > pm ? am : pm;
    } else {
        const int32_t usage = layout_text_width(f, "Usage");
        if (usage > w) w = usage;
    }
    return (int16_t)w;
}

// ─── The tuned designs ───────────────────────────────────────────────────────

// Fonts a title steps down through, largest first.
static const lv_font_t* const TITLE_FONTS[] = {
    &font_tiempos_56, &font_tiempos_34, &font_styrene_24, &font_styrene_16, &font_styrene_14,
};
#define TITLE_FONT_COUNT (int)(sizeof(TITLE_FONTS) / sizeof(TITLE_FONTS[0]))

// Text sizes the pairing hint and the period line step down through.
static const lv_font_t* const TEXT_FONTS[] = {
    &font_styrene_48, &font_styrene_28, &font_styrene_20, &font_styrene_14, &font_styrene_12,
};
#define TEXT_FONT_COUNT (int)(sizeof(TEXT_FONTS) / sizeof(TEXT_FONTS[0]))

// The header and the status line: what sits against the panel's edges.
struct Chrome {
    int16_t margin;                // header inset from the side edges
    int16_t logo_y, title_y, batt_y;
    int16_t title_nudge;           // title right of centre, balancing the logo
    bool    small_icons;
    int8_t  title_font;            // first TITLE_FONTS entry tried
    const lv_font_t* status_font;
    int16_t status_inset;          // status line's distance from the bottom
    int16_t band_min, band_max;    // space between the cards and the status line
};

static const Chrome CHROME_LARGE = {
    .margin = 20, .logo_y = 20, .title_y = 30, .batt_y = 30, .title_nudge = 16,
    .small_icons = false, .title_font = 0,
    .status_font = &font_mono_32, .status_inset = 15, .band_min = 12, .band_max = 48,
};
static const Chrome CHROME_SMALL = {
    .margin = 8, .logo_y = 2, .title_y = 4, .batt_y = 10, .title_nudge = 8,
    .small_icons = true, .title_font = 1,
    .status_font = &font_mono_18, .status_inset = 10, .band_min = 6, .band_max = 24,
};

// The cards: type, padding and vertical rhythm.
struct Design {
    const Chrome* chrome;
    int16_t top;                   // first card's top edge, under this chrome
    int16_t margin;                // card inset from the side edges
    int16_t card_h, gap;
    int16_t bar_y, bar_h, reset_y; // inside the card padding
    int16_t pad_x, pad_y, pill_pad_x, pill_pad_y;
    const lv_font_t *pct, *ent_pct, *pill, *reset, *pace;
    int16_t pair_y[4];             // pairing lines' tops, from the first card's top
};

// 480x480 (AMOLED-2.16).
static const Design LARGE = {
    .chrome = &CHROME_LARGE, .top = 100, .margin = 20, .card_h = 150, .gap = 16,
    .bar_y = 56, .bar_h = 24, .reset_y = 94,
    .pad_x = 16, .pad_y = 12, .pill_pad_x = 18, .pill_pad_y = 6,
    .pct = &font_styrene_48, .ent_pct = &font_tiempos_56, .pill = &font_styrene_28,
    .reset = &font_styrene_28, .pace = &font_styrene_16,
    .pair_y = { 40, 120, 160, 230 },
};
// 368x448 (AMOLED-1.8): the large type on shorter cards. They start just
// low enough for the corner Clawd's jump to clear the top edge from a seat
// that still sits above the first card.
static const Design COMPACT = {
    .chrome = &CHROME_LARGE, .top = 96, .margin = 20, .card_h = 130, .gap = 12,
    .bar_y = 48, .bar_h = 24, .reset_y = 78,
    .pad_x = 16, .pad_y = 12, .pill_pad_x = 18, .pill_pad_y = 6,
    .pct = &font_styrene_48, .ent_pct = &font_tiempos_56, .pill = &font_styrene_28,
    .reset = &font_styrene_28, .pace = &font_styrene_16,
    .pair_y = { 40, 120, 160, 230 },
};
// 240x240 (LCD-1.54): type two steps down, cards about half as tall.
static const Design SMALL = {
    .chrome = &CHROME_SMALL, .top = 44, .margin = 8, .card_h = 74, .gap = 6,
    .bar_y = 30, .bar_h = 12, .reset_y = 46,
    .pad_x = 10, .pad_y = 6, .pill_pad_x = 8, .pill_pad_y = 2,
    .pct = &font_styrene_24, .ent_pct = &font_tiempos_34, .pill = &font_styrene_14,
    .reset = &font_styrene_14, .pace = &font_styrene_12,
    .pair_y = { 12, 56, 80, 112 },
};

static const Design* const DESIGNS[] = { &LARGE, &COMPACT, &SMALL };

#define TITLE_GAP   4   // title keeps this far from the logo and the battery
#define PILL_GAP    4   // the percentage keeps this far from its pill
#define ROUND_INSET 6   // content keeps this far inside a round panel's edge

// ─── Pieces ──────────────────────────────────────────────────────────────────

static int16_t logo_slot(const Layout& L) { return L.small_icons ? LAYOUT_LOGO_SLOT_SMALL : LAYOUT_LOGO_SLOT; }
static int16_t logo_w(const Layout& L)    { return L.small_icons ? LAYOUT_LOGO_W_SMALL : LAYOUT_LOGO_W; }
static int16_t batt_w(const Layout& L)    { return L.small_icons ? LAYOUT_BATT_W_SMALL : LAYOUT_BATT_W; }

static int16_t stack_bottom(const Layout& L) { return L.card[1].y + L.card[1].h; }
static int16_t status_top(const Layout& L) {
    return L.h + L.status_y - line_h(L.status_font);
}

// Header row: logo against `left`, battery against `right`, rows `dy`
// below the chrome's own.
static void place_header(Layout& L, const Chrome& c, int16_t left, int16_t right, int16_t dy) {
    L.small_icons = c.small_icons;
    L.logo_x = left;
    L.logo_y = c.logo_y + dy;
    L.batt_x = right - batt_w(L);
    L.batt_y = c.batt_y + dy;
    L.status_font = c.status_font;
}

// The two cards: `w` wide from `x`, the first at `top`.
static void place_cards(Layout& L, const Design& d, int16_t x, int16_t w, int16_t top) {
    L.card[0] = { x, top, w, d.card_h };
    L.card[1] = { x, (int16_t)(top + d.card_h + d.gap), w, d.card_h };
    L.pad_x = d.pad_x;           L.pad_y = d.pad_y;
    L.bar_y = d.bar_y;           L.bar_h = d.bar_h;
    L.reset_y = d.reset_y;
    L.pill_pad_x = d.pill_pad_x; L.pill_pad_y = d.pill_pad_y;
    L.pct_font = d.pct;          L.ent_pct_font = d.ent_pct;
    L.pill_font = d.pill;        L.reset_font = d.reset;
    L.pace_font = d.pace;
    L.pills = PILL_TEXT;
}

// Both cards' rows fit: the percentage beside its pill (the spend-capped
// account's number and "%" beside "Spending"), and the widest reset line.
static bool cards_fit(const Layout& L) {
    const int16_t inner = L.card[0].w - 2 * L.pad_x;
    const int16_t pill = PILL_GAP + 2 * L.pill_pad_x;
    const char* const usage_pills[] = { L.pills[0], L.pills[1], L.pills[3] };
    const int16_t usage = layout_text_width(L.pct_font, "100%") + pill + widest(L.pill_font, usage_pills, 3);
    const int16_t spend = layout_text_width(L.ent_pct_font, "100") + 4
                        + layout_text_width(L.reset_font, "%") + pill
                        + layout_text_width(L.pill_font, L.pills[2]);
    const int n = sizeof(RESET_TEXT_WIDEST) / sizeof(RESET_TEXT_WIDEST[0]);
    return usage <= inner && spend <= inner && widest(L.reset_font, RESET_TEXT_WIDEST, n) <= inner;
}

// Title fonts: per clock format, the largest that fits between the logo
// and the battery around title_cx, centred on the chrome's title line.
static void fit_title(Layout& L, const Chrome& c) {
    const int16_t top = L.logo_y + c.title_y - c.logo_y;
    const int16_t mid = 2 * top + line_h(TITLE_FONTS[c.title_font]);
    const int16_t left = L.title_cx - (L.logo_x + logo_w(L));
    const int16_t right = L.batt_x - L.title_cx;
    const int16_t room = 2 * ((left < right ? left : right) - TITLE_GAP);
    for (int fmt = 0; fmt < 2; fmt++) {
        L.title_font[fmt] = nullptr;
        for (int i = c.title_font; i < TITLE_FONT_COUNT; i++) {
            if (widest_clock(TITLE_FONTS[i], fmt == 1) <= room) {
                L.title_font[fmt] = TITLE_FONTS[i];
                break;
            }
        }
        L.title_y[fmt] = L.title_font[fmt] ? (mid - line_h(L.title_font[fmt])) / 2 : top;
    }
}

// Lines `text` wraps to at width `w`, breaking at spaces like LVGL.
static int wrapped_lines(const lv_font_t* f, const char* text, int16_t w) {
    if (layout_text_width(f, text) <= w) return 1;
    char line[64] = "", word[32];
    int lines = 1;
    for (const char* p = text; *p;) {
        int n = 0;
        while (*p == ' ') p++;
        while (*p && *p != ' ' && n < (int)sizeof(word) - 1) word[n++] = *p++;
        word[n] = 0;
        if (!n) break;
        char next[64];
        snprintf(next, sizeof(next), line[0] ? "%s %s" : "%s%s", line, word);
        if (line[0] && layout_text_width(f, next) > w) {
            lines++;
            snprintf(line, sizeof(line), "%s", word);
        } else {
            snprintf(line, sizeof(line), "%s", next);
        }
    }
    return lines;
}

// Pairing hint: the heading in TEXT_FONTS[t], the steps one size down, the
// note two (or the smallest), for the largest t whose widest step fits
// pair_w (else the smallest t, wrapping). The lines sit at the design's
// offsets below the first card's top, lower where a wrapped line above
// needs the room.
static void fit_pairing(Layout& L, const Design& d) {
    static const char* const* const SETS[] = { PAIR_TEXT_PWR, PAIR_TEXT_TOUCH, PAIR_TEXT_CLEARED };
    int t = 0;
    for (; t + 2 < TEXT_FONT_COUNT; t++) {
        int16_t w = 0;
        for (const char* const* s : SETS) {
            const int16_t sw = widest(TEXT_FONTS[t + 1], s + 1, 2);
            if (sw > w) w = sw;
        }
        if (w <= L.pair_w) break;
    }
    L.pair_font[0] = TEXT_FONTS[t];
    L.pair_font[1] = L.pair_font[2] = TEXT_FONTS[t + 1];
    L.pair_font[3] = TEXT_FONTS[t + 2 < TEXT_FONT_COUNT ? t + 2 : TEXT_FONT_COUNT - 1];
    L.pair_y = L.card[0].y;
    L.pair_wrap = false;
    int16_t shift = 0, bottom = 0;
    for (int i = 0; i < 4; i++) {
        int lines = 1;
        for (const char* const* s : SETS) {
            const int n = wrapped_lines(L.pair_font[i], i < 3 ? s[i] : PAIR_TEXT_HOLDING, L.pair_w);
            if (n > lines) lines = n;
        }
        if (lines > 1) L.pair_wrap = true;
        int16_t y = d.pair_y[i] + shift;
        if (y < bottom) { shift += bottom - y; y = bottom; }
        L.pair_line_y[i] = y;
        bottom = y + lines * line_h(L.pair_font[i]);
    }
}

// The spend-capped account's period line steps down from the reset font
// until it fits the card; where none fits, it drops the pace word (the bar
// and the date's colour still show the pace).
static void fit_period(Layout& L) {
    const int16_t inner = L.card[0].w - 2 * L.pad_x;
    L.period_short = false;
    for (const lv_font_t* f : TEXT_FONTS) {
        if (line_h(f) > line_h(L.reset_font)) continue;
        L.period_font = f;
        if (layout_text_width(f, PERIOD_TEXT_WIDEST) <= inner) return;
    }
    L.period_short = true;
    L.period_font = L.reset_font;
}

// ─── Rectangular panels: top-down ────────────────────────────────────────────

// The design as tuned: header at the top, cards from d.top, the status line
// at the bottom. False when the cards, the title or the status line don't
// fit this panel.
static bool place_flow(Layout& L, const Design& d, const Chrome** chrome) {
    const Chrome& c = *(*chrome = d.chrome);
    place_header(L, c, c.margin, L.w - c.margin, 0);
    L.title_cx = L.w / 2 + c.title_nudge;
    L.status_y = -c.status_inset;
    L.status_w = L.w - 2 * d.margin;
    L.pair_w = L.w - d.margin;     // free text keeps half a margin off the edges
    place_cards(L, d, d.margin, L.w - 2 * d.margin, d.top);
    fit_title(L, c);
    return cards_fit(L) && L.title_font[0] && status_top(L) - stack_bottom(L) >= c.band_min;
}

// Narrow panels give up decoration before information: short pill labels,
// then tighter spacing, then smaller pill and reset type, then smaller
// numbers.
static void fit_narrow(Layout& L) {
    if (cards_fit(L)) return;
    L.pills = PILL_TEXT_SHORT;
    if (cards_fit(L)) return;
    const int16_t m = 6;
    L.logo_x = m;
    L.batt_x = L.w - m - batt_w(L);
    for (LayoutRect& r : L.card) { r.x = m; r.w = L.w - 2 * m; }
    L.status_w = L.w - 2;          // the status line runs almost to the edges
    L.pair_w = L.w - m;
    L.pad_x = 6;
    L.pill_pad_x = 6;
    if (cards_fit(L)) return;
    L.pill_font = &font_styrene_12;
    L.reset_font = &font_styrene_12;
    if (cards_fit(L)) return;
    L.pct_font = &font_styrene_20;
    L.ent_pct_font = &font_styrene_20;
}

static int16_t card_gap(const Layout& L) { return L.card[1].y - (L.card[0].y + L.card[0].h); }

static void move_cards(Layout& L, int16_t dy, int16_t grow, int16_t gap_add) {
    const int16_t gap = card_gap(L) + gap_add;
    L.card[0].y += dy;
    L.card[0].h += grow;
    L.card[1].h += grow;
    L.card[1].y = L.card[0].y + L.card[0].h + gap;
}

// Height the design wasn't tuned for. A tall panel shares its spare height
// beyond band_max out (a quarter to each card, an eighth each to the gap
// and above the cards; the rest stays under them), so there's no empty
// band. One too short for the small design squeezes, in turn: the status
// line's inset down to 4 px, each card up to 10 px shorter (a slimmer bar,
// the reset line higher), the gap down to 4 px, the cards up to the header.
static void fit_height(Layout& L, const Chrome& c) {
    const int16_t band = status_top(L) - stack_bottom(L);
    if (band > c.band_max) {
        const int16_t spare = band - c.band_max;
        const int16_t grow = spare / 4;
        move_cards(L, spare / 8, grow, spare / 8);
        L.bar_y += grow / 3;
        L.reset_y += 2 * grow / 3;
        return;
    }
    int16_t need = c.band_min - band;
    if (need <= 0) return;
    auto take = [&need](int16_t room) -> int16_t {
        const int16_t t = room < need ? (room > 0 ? room : 0) : need;
        need -= t;
        return t;
    };
    L.status_y += take(-L.status_y - 4);
    const int16_t shrink = take(20) / 2;
    move_cards(L, 0, -shrink, 0);
    L.bar_y -= shrink / 5;
    L.bar_h -= shrink / 5;
    L.reset_y -= 3 * shrink / 5;
    move_cards(L, 0, 0, -take(card_gap(L) - 4));
    move_cards(L, -take(L.card[0].y - (L.logo_y + logo_slot(L))), 0, 0);
}

// ─── Round panels: centred ───────────────────────────────────────────────────

// Half the glass's width across rows y0..y1 of a circle of radius r centred
// at (cx, cy): the chord at the row farthest from the centre.
static int16_t chord_half(float cy, float r, int16_t y0, int16_t y1) {
    const float a = fabsf(y0 + 0.5f - cy), b = fabsf(y1 - 0.5f - cy);
    const float dy = a > b ? a : b;
    return dy >= r ? 0 : (int16_t)sqrtf(r * r - dy * dy);
}

// The cards centred on the circle, as wide as the glass allows at their
// corners, the header right above them and the status line right under
// them. False when the design can't fit this circle.
static bool place_round_with(Layout& L, const Design& d, const Chrome& c) {
    const float cx = L.w / 2.0f, cy = L.h / 2.0f;
    const float r = (L.w < L.h ? L.w : L.h) / 2.0f;
    const int16_t stack_h = 2 * d.card_h + d.gap;
    const int16_t top = (int16_t)(cy - stack_h / 2.0f + 0.5f);
    const int16_t card_half = chord_half(cy, r, top, top + stack_h) - ROUND_INSET;
    place_cards(L, d, (int16_t)(cx - card_half), 2 * card_half, top);

    // Header: right above the cards, logo and battery against the glass
    // across the slot's rows; it must fit the title between them.
    L.small_icons = c.small_icons;
    const int16_t slot = logo_slot(L);
    const int16_t y = top - slot - 2;
    const int16_t half = y < 0 ? 0 : chord_half(cy, r, y, y + slot) - ROUND_INSET;
    place_header(L, c, (int16_t)(cx - half), (int16_t)(cx + half), y - c.logo_y);
    L.title_cx = (L.logo_x + logo_w(L) + L.batt_x) / 2;
    const int16_t room = L.batt_x - (L.logo_x + logo_w(L)) - 2 * TITLE_GAP;

    // Status line just under the cards, as wide as the chord at its bottom.
    const int16_t s_top = stack_bottom(L) + c.band_min;
    const int16_t s_bottom = s_top + line_h(c.status_font);
    L.status_y = s_bottom - L.h;
    L.status_w = 2 * (chord_half(cy, r, s_top, s_bottom) - ROUND_INSET);
    L.pair_w = L.card[0].w;

    const int n = sizeof(STATUS_TEXT_REQUIRED) / sizeof(STATUS_TEXT_REQUIRED[0]);
    return y >= 0 && room >= widest_clock(TITLE_FONTS[c.title_font], false) && cards_fit(L)
        && widest(c.status_font, STATUS_TEXT_REQUIRED, n) <= L.status_w;
}

// The design's own chrome, else the small one: the circle's top and bottom
// are narrow, the cards keep their type either way.
static bool place_round(Layout& L, const Design& d, const Chrome** chrome) {
    *chrome = d.chrome;
    if (place_round_with(L, d, *d.chrome)) return true;
    if (d.chrome == &CHROME_SMALL) return false;
    *chrome = &CHROME_SMALL;
    return place_round_with(L, d, CHROME_SMALL);
}

// ─── Entry point ─────────────────────────────────────────────────────────────

Layout layout_compute(const LayoutShape& shape) {
    Layout L = {};
    L.w = shape.w;
    L.h = shape.h;

    // The largest design that fits; the small one is kept regardless and
    // adapted to the panel below.
    const Design* d = nullptr;
    const Chrome* c = nullptr;
    for (const Design* each : DESIGNS) {
        d = each;
        if (shape.round ? place_round(L, *d, &c) : place_flow(L, *d, &c)) break;
    }
    if (!shape.round) {
        fit_narrow(L);
        fit_height(L, *c);
    }
    fit_title(L, *c);
    fit_pairing(L, *d);
    fit_period(L);
    return L;
}
