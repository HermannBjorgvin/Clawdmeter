// button_action: the values the app sends (`k1`..`k3`) and what they do.
//   pio test -d firmware -e native -f test_button_action
#include <unity.h>
#include "button_action.h"

void setUp(void) {}
void tearDown(void) {}

static void assert_key(int32_t v, uint8_t key, uint8_t mods, bool hold) {
    const ButtonBinding b = button_action_decode(v);
    TEST_ASSERT_EQUAL(BTN_KIND_KEY, b.kind);
    TEST_ASSERT_EQUAL_HEX8(key, b.key);
    TEST_ASSERT_EQUAL_HEX8(mods, b.modifiers);
    TEST_ASSERT_EQUAL(hold, b.hold);
    TEST_ASSERT_FALSE(b.repeat);
}

static void test_presets(void) {
    assert_key(BTN_ACT_SPACE, 0x2C, 0x00, true);       // push to talk: held
    assert_key(BTN_ACT_SHIFT_TAB, 0x2B, 0x02, false);
    assert_key(BTN_ACT_TAB, 0x2B, 0x00, false);
    assert_key(BTN_ACT_ENTER, 0x28, 0x00, false);
    assert_key(BTN_ACT_ESCAPE, 0x29, 0x00, false);
    TEST_ASSERT_EQUAL(BTN_KIND_NONE, button_action_decode(BTN_ACT_NONE).kind);
    TEST_ASSERT_EQUAL(BTN_KIND_BRIGHTNESS, button_action_decode(BTN_ACT_BRIGHTNESS).kind);
}

static void test_chords(void) {
    // Ctrl+Shift+M, tapped (Teams mute).
    assert_key(0x10000 | (0x3 << 8) | 0x10, 0x10, 0x03, false);
    // Ctrl+Space held (push to talk).
    assert_key(0x10000 | 0x1000 | (0x1 << 8) | 0x2C, 0x2C, 0x01, true);
    // Win+Alt+K: GUI 8 | Alt 4.
    assert_key(0x10000 | (0xC << 8) | 0x0E, 0x0E, 0x0C, false);
    // F24, the last usage in the report.
    assert_key(0x10000 | 0x73, 0x73, 0x00, false);
}

static void test_consumer(void) {
    ButtonBinding b = button_action_decode(0x20000 | HID_CONSUMER_PLAY_PAUSE);
    TEST_ASSERT_EQUAL(BTN_KIND_CONSUMER, b.kind);
    TEST_ASSERT_EQUAL_HEX16(0xCD, b.consumer);
    TEST_ASSERT_FALSE(b.repeat);
    b = button_action_decode(0x20000 | HID_CONSUMER_VOL_UP);
    TEST_ASSERT_TRUE(b.repeat);                          // volume repeats while held
    b = button_action_decode(0x20000 | HID_CONSUMER_VOL_DOWN);
    TEST_ASSERT_TRUE(b.repeat);
    b = button_action_decode(0x20000 | HID_CONSUMER_MUTE);
    TEST_ASSERT_FALSE(b.repeat);
}

static void test_invalid(void) {
    const int32_t bad[] = {
        -1, 7, 99, 0xFFFF,
        0x10000,                       // keyboard, usage 0
        0x10000 | 0x03,                // keyboard error usage
        0x10000 | 0x74,                // past F24
        0x10000 | 0x2000 | 0x04,       // a bit outside the fields
        0x20000,                       // consumer usage 0
        0x20000 | 0x400,               // past the consumer range
        0x30000 | 0x04,                // both kinds
        0x40000,
    };
    for (int32_t v : bad) {
        TEST_ASSERT_FALSE_MESSAGE(button_action_valid(v), "should be invalid");
        TEST_ASSERT_EQUAL(BTN_ACT_SPACE, button_action_clamp(v, BTN_ACT_SPACE));
        TEST_ASSERT_EQUAL(BTN_KIND_NONE, button_action_decode(v).kind);
    }
    TEST_ASSERT_EQUAL(BTN_ACT_TAB, button_action_clamp(BTN_ACT_TAB, BTN_ACT_SPACE));
    TEST_ASSERT_EQUAL(0x10F10, button_action_clamp(0x10F10, BTN_ACT_SPACE));
    TEST_ASSERT_EQUAL(0x203FF, button_action_clamp(0x203FF, BTN_ACT_SPACE));
}

static void test_keyboard_report(void) {
    KeyboardHeld held = {};
    uint8_t r[8];
    keyboard_report(&held, 0, 0, r);
    const uint8_t empty[8] = {0};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(empty, r, 8);
    // Primary holds Ctrl+Space; Secondary taps Shift+Tab on top of it.
    held.mods[0] = 0x01;
    held.keys[0] = 0x2C;
    keyboard_report(&held, 0x02, 0x2B, r);
    const uint8_t both[8] = {0x03, 0, 0x2C, 0x2B, 0, 0, 0, 0};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(both, r, 8);
    // After the tap the held chord is still down.
    keyboard_report(&held, 0, 0, r);
    const uint8_t held_only[8] = {0x01, 0, 0x2C, 0, 0, 0, 0, 0};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(held_only, r, 8);
    // The same key twice is listed once.
    held.keys[1] = 0x2C;
    keyboard_report(&held, 0, 0x2C, r);
    TEST_ASSERT_EQUAL_HEX8(0x2C, r[2]);
    TEST_ASSERT_EQUAL_HEX8(0x00, r[3]);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_presets);
    RUN_TEST(test_chords);
    RUN_TEST(test_consumer);
    RUN_TEST(test_invalid);
    RUN_TEST(test_keyboard_report);
    return UNITY_END();
}
