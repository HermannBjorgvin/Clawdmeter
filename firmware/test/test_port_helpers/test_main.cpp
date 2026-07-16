#include <Arduino.h>
#include <unity.h>

#include "boards/esp32_2432s024c/power_button.h"
#include "boards/esp32_2432s024c/touch_mapping.h"

void test_touch_mapping_keeps_portrait_axes(void) {
    TouchPoint point = map_touch_to_portrait(40, 250);

    TEST_ASSERT_EQUAL_UINT16(40, point.x);
    TEST_ASSERT_EQUAL_UINT16(250, point.y);
}

void test_touch_mapping_clamps_edges(void) {
    TouchPoint point = map_touch_to_portrait(900, 900);

    TEST_ASSERT_EQUAL_UINT16(239, point.x);
    TEST_ASSERT_EQUAL_UINT16(319, point.y);
}

void test_short_press_emits_only_short_event(void) {
    PowerButtonState state{};
    update_power_button(state, false, 0);
    update_power_button(state, true, 100);
    PowerButtonEvents events = update_power_button(state, false, 500);

    TEST_ASSERT_TRUE(events.pressed);
    TEST_ASSERT_TRUE(events.released);
    TEST_ASSERT_FALSE(events.long_pressed);
}

void test_long_press_does_not_emit_short_event(void) {
    PowerButtonState state{};
    update_power_button(state, true, 100);
    PowerButtonEvents held = update_power_button(state, true, 1700);
    PowerButtonEvents released = update_power_button(state, false, 3200);

    TEST_ASSERT_TRUE(held.long_pressed);
    TEST_ASSERT_FALSE(released.pressed);
    TEST_ASSERT_TRUE(released.released);
}

void setup() {
    delay(2000);
    UNITY_BEGIN();
    RUN_TEST(test_touch_mapping_keeps_portrait_axes);
    RUN_TEST(test_touch_mapping_clamps_edges);
    RUN_TEST(test_short_press_emits_only_short_event);
    RUN_TEST(test_long_press_does_not_emit_short_event);
    UNITY_END();
}

void loop() {}
