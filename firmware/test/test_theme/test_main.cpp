// theme_clawd_art: Clawd's sprite follows TH_CLAWD by a palette swap, and
// the default palette leaves the official art exactly as it is.
//   pio test -d firmware -e native -f test_theme
#include <unity.h>
#include "theme_palette.h"
#include "clawd_anim.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

// The live palettes the sprites draw with (clawd_anim.cpp), copied out.
static uint16_t art[64][CLAWD_PALETTE_SIZE];
static void snapshot(void) {
    for (int i = 0; i < clawd_anim_count() && i < 64; i++)
        memcpy(art[i], clawd_anim_at(i)->palette, sizeof(art[i]));
}

static void test_default_clawd_keeps_the_official_colours(void) {
    TEST_ASSERT_EQUAL_HEX16(CLAWD_ART_BODY, theme_rgb565(THEME_DEFAULTS[TH_CLAWD]));
    TEST_ASSERT_TRUE(clawd_anim_count() > 0 && clawd_anim_count() <= 64);
    snapshot();
    bool body = false;
    for (int i = 0; i < clawd_anim_count(); i++)
        for (int c = 0; c < CLAWD_PALETTE_SIZE; c++) {
            TEST_ASSERT_EQUAL_HEX16_MESSAGE(art[i][c], theme_clawd_art(art[i][c], THEME_DEFAULTS[TH_CLAWD]),
                                            clawd_anim_at(i)->name);
            body |= art[i][c] == CLAWD_ART_BODY;
        }
    TEST_ASSERT_TRUE_MESSAGE(body, "the art still uses the body orange this recolours");
}

// A new TH_CLAWD repaints every animation's body, and the default brings
// the official colours back.
static void test_sprites_follow_the_palette(void) {
    snapshot();
    uint32_t p[TH_COUNT];
    memcpy(p, THEME_DEFAULTS, sizeof(p));
    p[TH_CLAWD] = 0x647d99;
    theme_palette_set(p);
    clawd_anim_recolor();
    for (int i = 0; i < clawd_anim_count(); i++)
        for (int c = 0; c < CLAWD_PALETTE_SIZE; c++)
            TEST_ASSERT_EQUAL_HEX16(theme_clawd_art(art[i][c], 0x647d99), clawd_anim_at(i)->palette[c]);
    TEST_ASSERT_EQUAL_HEX16(theme_rgb565(0x647d99), clawd_anim("walking")->palette[1]);
    theme_palette_set(THEME_DEFAULTS);
    clawd_anim_recolor();
    for (int i = 0; i < clawd_anim_count(); i++)
        TEST_ASSERT_EQUAL_HEX16_ARRAY(art[i], clawd_anim_at(i)->palette, CLAWD_PALETTE_SIZE);
}

static void test_new_clawd_recolours_only_his_body(void) {
    const uint32_t blue = 0x647d99;
    const uint16_t body = theme_clawd_art(CLAWD_ART_BODY, blue);
    const uint16_t shade = theme_clawd_art(CLAWD_ART_SHADE, blue);
    TEST_ASSERT_EQUAL_HEX16(theme_rgb565(blue), body);
    // The shaded side is darker in every channel, as in the art.
    TEST_ASSERT_TRUE((shade >> 11) < (body >> 11));
    TEST_ASSERT_TRUE(((shade >> 5) & 0x3f) < ((body >> 5) & 0x3f));
    TEST_ASSERT_TRUE((shade & 0x1f) < (body & 0x1f));
    // Eyes, outlines and props keep their own colours.
    TEST_ASSERT_EQUAL_HEX16(0x0000, theme_clawd_art(0x0000, blue));
    TEST_ASSERT_EQUAL_HEX16(0x10A2, theme_clawd_art(0x10A2, blue));
    TEST_ASSERT_EQUAL_HEX16(0xBAA6, theme_clawd_art(0xBAA6, blue));   // the basketball
    // Black stays black, white stays bright.
    TEST_ASSERT_EQUAL_HEX16(0x0000, theme_clawd_art(CLAWD_ART_SHADE, 0x000000));
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, theme_clawd_art(CLAWD_ART_BODY, 0xffffff));
}

static void test_rgb565(void) {
    TEST_ASSERT_EQUAL_HEX16(0xF800, theme_rgb565(0xff0000));
    TEST_ASSERT_EQUAL_HEX16(0x07E0, theme_rgb565(0x00ff00));
    TEST_ASSERT_EQUAL_HEX16(0x001F, theme_rgb565(0x0000ff));
    TEST_ASSERT_EQUAL_HEX16(0x10A2, theme_rgb565(0x141413));   // the face's eyes, as in the art
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_default_clawd_keeps_the_official_colours);
    RUN_TEST(test_sprites_follow_the_palette);
    RUN_TEST(test_new_clawd_recolours_only_his_body);
    RUN_TEST(test_rgb565);
    return UNITY_END();
}
