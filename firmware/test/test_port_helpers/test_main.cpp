#include <Arduino.h>
#include <unity.h>

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

void setup() {
    delay(2000);
    UNITY_BEGIN();
    RUN_TEST(test_touch_mapping_keeps_portrait_axes);
    RUN_TEST(test_touch_mapping_clamps_edges);
    UNITY_END();
}

void loop() {}
