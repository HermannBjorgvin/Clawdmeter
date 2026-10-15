// layout_compute() on every supported panel shape, measured with the real
// fonts (fonts.c): everything stays on the glass, nothing overlaps, every
// string the meters can show fits its slot, and the boards on main keep the
// exact layout they were tuned with.
#include <unity.h>
#include <stdio.h>
#include <math.h>
#include <initializer_list>
#include "../../src/layout.cpp"

struct Panel { const char* name; int16_t w, h; bool round; };

// Every supported shape: the boards on main, then the open ports' panels.
static const Panel PANELS[] = {
    { "AMOLED-2.16 / LCD-4",  480, 480, false },
    { "AMOLED-1.8",           368, 448, false },
    { "AMOLED-2.06",          410, 502, false },
    { "LCD-1.54",             240, 240, false },
    { "CYD / M5Stack FIRE",   320, 240, false },
    { "CYD portrait / LCD-2", 240, 320, false },
    { "LilyGO T4-S3",         450, 600, false },
    { "T4-S3 landscape",      600, 450, false },
    { "T-Display S3",         170, 320, false },
    { "M5StickS3",            135, 240, false },
    { "AMOLED-1.32 / 1.43",   466, 466, true  },
    { "ePaper 1.54",          200, 200, false },
    { "LCD-1.83",             240, 284, false },
    { "Xingzhi Cube",         284, 240, false },
};

struct Box { int x, y, w, h; };

static char where[160];
static const Panel* P;
static Layout L;

static void fail(const char* what, Box b) {
    snprintf(where, sizeof(where), "%s %dx%d%s: %s (%d,%d %dx%d)", P->name, P->w, P->h,
             P->round ? " round" : "", what, b.x, b.y, b.w, b.h);
    TEST_FAIL_MESSAGE(where);
}

static bool on_glass(int x, int y) {
    if (x < 0 || y < 0 || x > P->w || y > P->h) return false;
    if (!P->round) return true;
    const float r = (P->w < P->h ? P->w : P->h) / 2.0f;
    const float dx = x - P->w / 2.0f, dy = y - P->h / 2.0f;
    return dx * dx + dy * dy <= r * r + 0.5f;
}

// The box is on the glass (a round panel: all four corners, `round_r` in
// from a rounded box's corners).
static void check_on_glass(const char* what, Box b, int round_r = 0) {
    const int c = round_r - (int)(round_r / 1.41421f);
    if (!on_glass(b.x + c, b.y + c) || !on_glass(b.x + b.w - c, b.y + c) ||
        !on_glass(b.x + c, b.y + b.h - c) || !on_glass(b.x + b.w - c, b.y + b.h - c))
        fail(what, b);
}

static bool overlap(Box a, Box b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

static void check_apart(const char* what, Box a, Box b) {
    if (overlap(a, b)) {
        snprintf(where, sizeof(where), "%s: (%d,%d %dx%d) vs (%d,%d %dx%d)", what,
                 a.x, a.y, a.w, a.h, b.x, b.y, b.w, b.h);
        fail(where, a);
    }
}

static int text_w(const lv_font_t* f, const char* s) { return layout_text_width(f, s); }
// Ink height of a line of digits or capitals: the line box minus the descender.
static int ink_h(const lv_font_t* f) { return f->line_height - f->base_line; }

static Box logo_art() {
    const int art_h = L.small_icons ? 32 : 48;   // clawd_still.h
    return { L.logo_x, L.logo_y + (logo_slot(L) - art_h) / 2, logo_w(L), art_h };
}
static Box battery() { return { L.batt_x, L.batt_y, batt_w(L), batt_w(L) }; }
static Box card(int i) { return { L.card[i].x, L.card[i].y, L.card[i].w, L.card[i].h }; }
static Box status_box(const char* text) {
    const int w = text_w(L.status_font, text), lh = line_h(L.status_font);
    return { (P->w - w) / 2, P->h + L.status_y - lh, w, lh };
}

// Clock strings for one format: every time of day, and "Usage" with the
// 24-hour clock.
static void each_title(bool h12, void (*fn)(const char*)) {
    char buf[12];
    for (int h = 0; h < 24; h++)
        for (int m = 0; m < 60; m++) {
            if (h12) snprintf(buf, sizeof(buf), "%d:%02d %s", h % 12 ? h % 12 : 12, m, h < 12 ? "AM" : "PM");
            else     snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
            fn(buf);
        }
    if (!h12) fn("Usage");
}

static int title_fmt;
static void check_title(const char* text) {
    const lv_font_t* f = L.title_font[title_fmt];
    const int w = text_w(f, text);
    const Box t = { L.title_cx - w / 2, L.title_y[title_fmt], w, line_h(f) };
    check_on_glass("title", t);
    check_apart("title vs corner Clawd", t, logo_art());
    check_apart("title vs battery", t, battery());
}

static void check_panel(const Panel& p) {
    P = &p;
    L = layout_compute({ p.w, p.h, p.round });

    // Header.
    check_on_glass("corner Clawd", logo_art());
    check_on_glass("battery", battery());
    check_apart("corner Clawd vs battery", logo_art(), battery());
    for (title_fmt = 0; title_fmt < 2; title_fmt++)
        if (L.title_font[title_fmt]) each_title(title_fmt == 1, check_title);
    if (p.w >= 200 && !L.title_font[0]) fail("no room for the title", { 0, 0, p.w, 0 });

    // Cards: on the glass, apart, below the header, above the status line.
    for (int i = 0; i < 2; i++) {
        check_on_glass("card", card(i), 8);
        check_apart("card vs corner Clawd", card(i), logo_art());
        check_apart("card vs battery", card(i), battery());
        for (title_fmt = 0; title_fmt < 2; title_fmt++) {
            const lv_font_t* f = L.title_font[title_fmt];
            if (!f) continue;
            check_apart("card vs title", card(i), { 0, L.title_y[title_fmt], p.w, ink_h(f) });
        }
    }
    check_apart("cards", card(0), card(1));

    // Inside a card.
    const int inner_w = L.card[0].w - 2 * L.pad_x, inner_h = L.card[0].h - 2 * L.pad_y;
    const int pill_w = 2 * L.pill_pad_x + 4;   // padding + PILL_GAP
    for (int i : { 0, 1, 3 })   // session, weekly, period: "100%" beside the pill
        if (text_w(L.pct_font, "100%") + pill_w + text_w(L.pill_font, L.pills[i]) > inner_w)
            fail("percentage vs pill", card(i == 0 ? 0 : 1));
    if (text_w(L.ent_pct_font, "100") + 4 + text_w(L.reset_font, "%") + pill_w
        + text_w(L.pill_font, L.pills[2]) > inner_w)
        fail("spending vs pill", card(0));
    for (const char* s : RESET_TEXT_WIDEST)
        if (text_w(L.reset_font, s) > inner_w) fail(s, card(0));
    if (text_w(L.period_font, L.period_short ? PERIOD_TEXT_SHORT_WIDEST : PERIOD_TEXT_WIDEST) > inner_w)
        fail("period line", card(1));
    if (p.w >= 240 && L.period_short) fail("period line without its pace word", card(1));
    if (1 + 2 * L.pill_pad_y + ink_h(L.pill_font) > L.bar_y) fail("pill vs bar", card(0));
    if (ink_h(L.pct_font) > L.bar_y) fail("percentage vs bar", card(0));
    if (L.bar_y + L.bar_h > L.reset_y) fail("bar vs reset line", card(0));
    if (L.reset_y + ink_h(L.reset_font) > inner_h) fail("reset line vs card bottom", card(0));

    // Status line: every state word fits, under the cards, on the glass.
    if (L.status_w > p.w) fail("status line wider than the panel", { 0, 0, L.status_w, 0 });
    for (const char* s : STATUS_TEXT_REQUIRED) {
        if (text_w(L.status_font, s) > L.status_w) fail(s, status_box(s));
        check_on_glass("status line", status_box(s));
        check_apart("status line vs cards", status_box(s), card(1));
    }

    // Pairing hint: every variant's lines on the glass, apart, under the
    // header and over the status line.
    static const char* const* const SETS[] = { PAIR_TEXT_PWR, PAIR_TEXT_TOUCH, PAIR_TEXT_CLEARED };
    const Box status = status_box(STATUS_TEXT_REQUIRED[0]);
    for (const char* const* set : SETS) {
        Box prev = { 0, 0, 0, 0 };
        for (int i = 0; i < 4; i++) {
            const char* s = i < 3 ? set[i] : PAIR_TEXT_HOLDING;
            const lv_font_t* f = L.pair_font[i];
            int w = text_w(f, s);
            int lines = 1;
            if (w > L.pair_w) {
                if (!L.pair_wrap) fail(s, { 0, L.pair_y + L.pair_line_y[i], w, 0 });
                lines = wrapped_lines(f, s, L.pair_w);
                w = L.pair_w;
            }
            const Box b = { (p.w - w) / 2, L.pair_y + L.pair_line_y[i], w, lines * line_h(f) };
            check_on_glass(s, b);
            check_apart("pairing line vs status line", b, status);
            check_apart("pairing line vs corner Clawd", b, logo_art());
            check_apart("pairing line vs battery", b, battery());
            if (i) check_apart("pairing lines", prev, b);
            prev = b;
        }
    }
}

static void test_every_panel_fits(void) {
    for (const Panel& p : PANELS) check_panel(p);
}

// The boards on main keep the layout they were tuned with.
static void test_large_480x480(void) {
    L = layout_compute({ 480, 480, false });
    TEST_ASSERT_EQUAL_INT16(20, L.card[0].x);   TEST_ASSERT_EQUAL_INT16(440, L.card[0].w);
    TEST_ASSERT_EQUAL_INT16(100, L.card[0].y);  TEST_ASSERT_EQUAL_INT16(150, L.card[0].h);
    TEST_ASSERT_EQUAL_INT16(266, L.card[1].y);
    TEST_ASSERT_EQUAL_INT16(56, L.bar_y);       TEST_ASSERT_EQUAL_INT16(24, L.bar_h);
    TEST_ASSERT_EQUAL_INT16(94, L.reset_y);
    TEST_ASSERT_EQUAL_PTR(&font_styrene_48, L.pct_font);
    TEST_ASSERT_EQUAL_PTR(&font_styrene_28, L.period_font);
    TEST_ASSERT_EQUAL_PTR(&font_tiempos_56, L.title_font[0]);
    TEST_ASSERT_EQUAL_PTR(&font_tiempos_56, L.title_font[1]);
    TEST_ASSERT_EQUAL_INT16(30, L.title_y[0]);  TEST_ASSERT_EQUAL_INT16(256, L.title_cx);
    TEST_ASSERT_EQUAL_INT16(20, L.logo_x);      TEST_ASSERT_EQUAL_INT16(20, L.logo_y);
    TEST_ASSERT_EQUAL_INT16(412, L.batt_x);     TEST_ASSERT_EQUAL_INT16(30, L.batt_y);
    TEST_ASSERT_EQUAL_PTR(&font_mono_32, L.status_font);
    TEST_ASSERT_EQUAL_INT16(-15, L.status_y);   TEST_ASSERT_EQUAL_INT16(440, L.status_w);
    TEST_ASSERT_EQUAL_PTR(&font_styrene_48, L.pair_font[0]);
    TEST_ASSERT_EQUAL_INT16(100, L.pair_y);
    TEST_ASSERT_EQUAL_INT16(230, L.pair_line_y[3]);
    TEST_ASSERT_EQUAL_PTR(PILL_TEXT, L.pills);
}

static void test_compact_368x448(void) {
    L = layout_compute({ 368, 448, false });
    TEST_ASSERT_EQUAL_INT16(96, L.card[0].y);   TEST_ASSERT_EQUAL_INT16(130, L.card[0].h);
    TEST_ASSERT_EQUAL_INT16(238, L.card[1].y);  TEST_ASSERT_EQUAL_INT16(328, L.card[0].w);
    TEST_ASSERT_EQUAL_INT16(48, L.bar_y);       TEST_ASSERT_EQUAL_INT16(78, L.reset_y);
    TEST_ASSERT_EQUAL_PTR(&font_styrene_48, L.pct_font);
    TEST_ASSERT_EQUAL_PTR(&font_tiempos_56, L.title_font[0]);
    TEST_ASSERT_EQUAL_PTR(&font_tiempos_34, L.title_font[1]);   // 12-hour clock: one size down
    TEST_ASSERT_EQUAL_PTR(&font_styrene_20, L.period_font);     // period line: one size down
    TEST_ASSERT_EQUAL_PTR(&font_styrene_28, L.pair_font[0]);
    TEST_ASSERT_EQUAL_INT16(-15, L.status_y);
}

static void test_large_410x502(void) {
    L = layout_compute({ 410, 502, false });
    TEST_ASSERT_EQUAL_INT16(100, L.card[0].y);  TEST_ASSERT_EQUAL_INT16(150, L.card[0].h);
    TEST_ASSERT_EQUAL_INT16(370, L.card[0].w);
    TEST_ASSERT_EQUAL_PTR(&font_styrene_48, L.pair_font[0]);
    TEST_ASSERT_EQUAL_PTR(&font_tiempos_34, L.title_font[1]);
    TEST_ASSERT_EQUAL_INT16(-15, L.status_y);
}

static void test_small_240x240(void) {
    L = layout_compute({ 240, 240, false });
    TEST_ASSERT_EQUAL_INT16(8, L.card[0].x);    TEST_ASSERT_EQUAL_INT16(224, L.card[0].w);
    TEST_ASSERT_EQUAL_INT16(44, L.card[0].y);   TEST_ASSERT_EQUAL_INT16(74, L.card[0].h);
    TEST_ASSERT_EQUAL_INT16(124, L.card[1].y);
    TEST_ASSERT_EQUAL_INT16(30, L.bar_y);       TEST_ASSERT_EQUAL_INT16(12, L.bar_h);
    TEST_ASSERT_EQUAL_INT16(46, L.reset_y);
    TEST_ASSERT_TRUE(L.small_icons);
    TEST_ASSERT_EQUAL_PTR(&font_tiempos_34, L.title_font[0]);
    TEST_ASSERT_EQUAL_PTR(&font_styrene_24, L.title_font[1]);
    TEST_ASSERT_EQUAL_INT16(4, L.title_y[0]);   TEST_ASSERT_EQUAL_INT16(128, L.title_cx);
    TEST_ASSERT_EQUAL_INT16(208, L.batt_x);     TEST_ASSERT_EQUAL_INT16(10, L.batt_y);
    TEST_ASSERT_EQUAL_PTR(&font_mono_18, L.status_font);
    TEST_ASSERT_EQUAL_INT16(-10, L.status_y);
    TEST_ASSERT_EQUAL_PTR(&font_styrene_20, L.pair_font[0]);
    TEST_ASSERT_EQUAL_INT16(112, L.pair_line_y[3]);
    TEST_ASSERT_EQUAL_PTR(&font_styrene_14, L.period_font);
}

// Kerned, per-glyph-rounded widths, as lv_font_get_glyph_width sums them.
static void test_text_width(void) {
    TEST_ASSERT_EQUAL_INT32(371, layout_text_width(&font_styrene_28, "for 3 seconds, then release"));
    TEST_ASSERT_EQUAL_INT32(0, layout_text_width(&font_styrene_28, ""));
}

static void print_layouts(void) {
    for (const Panel& p : PANELS) {
        L = layout_compute({ p.w, p.h, p.round });
        char msg[200];
        snprintf(msg, sizeof(msg), "%-22s %3dx%-3d cards x%d y%d/%d %dx%d  status y%d w%d  title %s  pills %s",
                 p.name, p.w, p.h, L.card[0].x, L.card[0].y, L.card[1].y, L.card[0].w, L.card[0].h,
                 L.status_y, L.status_w, L.title_font[0] ? "yes" : "hidden", L.pills[0]);
        TEST_MESSAGE(msg);
    }
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_every_panel_fits);
    RUN_TEST(test_large_480x480);
    RUN_TEST(test_compact_368x448);
    RUN_TEST(test_large_410x502);
    RUN_TEST(test_small_240x240);
    RUN_TEST(test_text_width);
    RUN_TEST(print_layouts);
    return UNITY_END();
}
