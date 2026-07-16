#include <Arduino.h>
#include <unity.h>

#include "boards/esp32_2432s024c/power_button.h"
#include "boards/esp32_2432s024c/display_color_order.h"
#include "boards/esp32_2432s024c/touch_mapping.h"
#include "serial_protocol.h"
#include "splash_layout.h"
#include "ui_layout.h"
#include "usage_view_state.h"

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

void test_240x320_layout_fits_both_panels(void) {
    UiLayoutMetrics metrics = compute_ui_layout_metrics(240, 320);
    const int bottom = metrics.content_y +
        (2 * metrics.usage_panel_h) + metrics.usage_panel_gap;

    TEST_ASSERT_LESS_OR_EQUAL_INT(276, bottom);
    TEST_ASSERT_TRUE(metrics.small_display);
    TEST_ASSERT_EQUAL_INT(18, metrics.status_font_px);
}

void test_existing_layout_breakpoints_remain_distinct(void) {
    UiLayoutMetrics compact = compute_ui_layout_metrics(368, 448);
    UiLayoutMetrics large = compute_ui_layout_metrics(480, 480);

    TEST_ASSERT_FALSE(compact.small_display);
    TEST_ASSERT_EQUAL_INT(130, compact.usage_panel_h);
    TEST_ASSERT_EQUAL_INT(150, large.usage_panel_h);
}

void test_small_no_psram_splash_keeps_heap_headroom(void) {
    TEST_ASSERT_EQUAL_INT(8, compute_splash_cell(240, false));
    TEST_ASSERT_EQUAL_INT(10, compute_splash_cell(480, false));
    TEST_ASSERT_EQUAL_INT(12, compute_splash_cell(240, true));
}

void test_serial_protocol_recognizes_usage_json(void) {
    TEST_ASSERT_EQUAL(SERIAL_LINE_USAGE_JSON,
                      classify_serial_line("{\"s\":12.5,\"w\":34.0}"));
}

void test_serial_protocol_recognizes_identify_command(void) {
    TEST_ASSERT_EQUAL(SERIAL_LINE_IDENTIFY, classify_serial_line("identify"));
}

void test_st7789_portrait_mode_uses_bgr_color_order(void) {
    TEST_ASSERT_EQUAL_HEX8(0xC8, st7789_portrait_bgr_madctl());
}

void test_fresh_serial_data_selects_live_usage_without_ble(void) {
    TEST_ASSERT_EQUAL(
        USAGE_VIEW_LIVE,
        select_usage_view_state(false, true, 1000, 950, 90000)
    );
}

void test_stale_data_without_ble_selects_waiting_view(void) {
    TEST_ASSERT_EQUAL(
        USAGE_VIEW_WAITING,
        select_usage_view_state(false, true, 100000, 1000, 90000)
    );
}

void setup() {
    delay(2000);
    UNITY_BEGIN();
    RUN_TEST(test_touch_mapping_keeps_portrait_axes);
    RUN_TEST(test_touch_mapping_clamps_edges);
    RUN_TEST(test_short_press_emits_only_short_event);
    RUN_TEST(test_long_press_does_not_emit_short_event);
    RUN_TEST(test_240x320_layout_fits_both_panels);
    RUN_TEST(test_existing_layout_breakpoints_remain_distinct);
    RUN_TEST(test_small_no_psram_splash_keeps_heap_headroom);
    RUN_TEST(test_serial_protocol_recognizes_usage_json);
    RUN_TEST(test_serial_protocol_recognizes_identify_command);
    RUN_TEST(test_st7789_portrait_mode_uses_bgr_color_order);
    RUN_TEST(test_fresh_serial_data_selects_live_usage_without_ble);
    RUN_TEST(test_stale_data_without_ble_selects_waiting_view);
    UNITY_END();
}

void loop() {}
