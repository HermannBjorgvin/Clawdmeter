#pragma once

// The Fable weekly limit, drawn inside the weekly bar (payload `wf`, Fable's
// own % of its limit). Pure: no LVGL, so it runs in the native unit tests.
//
// Fable's weekly limit is half the overall weekly limit, so the overlay is
// fable_pct x FABLE_WEEKLY_SHARE_PCT % of the bar: a used-up Fable limit fills
// it to the halfway point. The app has the same constant,
// `usage::FABLE_WEEKLY_SHARE_PCT` (docs/protocol.md).
#define FABLE_WEEKLY_SHARE_PCT 50

// The overlay's colour until the app sends one (`fc`, kept in NVS by
// clawd_prefs.cpp; theme_fable_hex() in theme_palette.h): the Dark theme's
// violet, at the brand accents' saturation and lightness, about 4:1 on the
// dark bar track. Each app theme has its own (`themes.rs`); it travels
// apart from `th` so the palette stays the eleven colours older firmware
// accepts.
#define FABLE_COLOR_DEFAULT 0x9775c7

// The overlay's shape (ui.cpp place_fable): its left end follows the bar's
// rounded end, its right end is straight, and it never runs past the weekly
// fill; at the fill's end it takes the fill's rounded corner. fable_fill_w()
// is lv_bar's own indicator width for `value` (0-100) on a bar `bar_w` wide;
// fable_overlay_w() is the overlay's width, clamped to that fill.
//
// Fable usage is part of the weekly usage, so a week spent only on Fable
// fills the whole fill. `wf` and `w` are rounded apart (week 31 %, Fable
// 61 %: 30.5 % of the bar), so an overlay that stops within
// FABLE_SNAP_PCT points of the bar, or the fill's corner radius if that's
// wider, from the fill's end runs to it instead of leaving a sliver of fill
// beside a straight end. The app snaps the same way (`ui/overview.rs`).
#define FABLE_SNAP_PCT 1

// How much of the bar the overlay covers, 0..FABLE_WEEKLY_SHARE_PCT (Fable
// past its limit stays at the halfway point). -1 when there's nothing to
// draw: `wf` absent (unknown, or no Fable limit) parses to -1.
static inline float fable_bar_pct(int fable_pct) {
    if (fable_pct < 0) return -1.0f;
    const int pct = fable_pct > 100 ? 100 : fable_pct;
    return pct * (FABLE_WEEKLY_SHARE_PCT / 100.0f);
}

static inline int fable_fill_w(int bar_w, int value) {
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    return bar_w * value / 100;
}

// 0 = nothing to draw. `radius`: the fill's corner radius.
static inline int fable_overlay_w(int fable_pct, int bar_w, int fill_w, int radius) {
    const float share = fable_bar_pct(fable_pct);
    if (share < 0) return 0;
    const int w = (int)(share * bar_w / 100.0f + 0.5f);
    if (w < 1) return 0;
    const int pct_px = (bar_w * FABLE_SNAP_PCT + 50) / 100;
    const int snap = pct_px > radius ? pct_px : radius;
    return w < fill_w - snap ? w : fill_w;
}
