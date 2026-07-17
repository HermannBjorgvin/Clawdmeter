#include <Arduino.h>
#include <unity.h>

#include "boards/esp32_2432s024c/power_button.h"
#include "boards/esp32_2432s024c/display_color_order.h"
#include "boards/esp32_2432s024c/touch_mapping.h"
#include "activity_freshness.h"
#include "dashboard_payload.h"
#include "dashboard_carousel.h"
#include "serial_protocol.h"
#include "splash_layout.h"
#include "ui.h"
#include "ui_layout.h"
#include "usage_view_state.h"

#include "../../src/dashboard_payload.cpp"
#include "../../src/ui.cpp"

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

void test_landscape_touch_mapping_rotates_with_usb_left(void) {
    TouchPoint top_left = map_touch_to_landscape(239, 319);
    TouchPoint top_right = map_touch_to_landscape(239, 0);
    TouchPoint bottom_left = map_touch_to_landscape(0, 319);
    TouchPoint bottom_right = map_touch_to_landscape(0, 0);
    TouchPoint center = map_touch_to_landscape(120, 160);
    TEST_ASSERT_EQUAL_UINT16(0, top_left.x);
    TEST_ASSERT_EQUAL_UINT16(0, top_left.y);
    TEST_ASSERT_EQUAL_UINT16(319, top_right.x);
    TEST_ASSERT_EQUAL_UINT16(0, top_right.y);
    TEST_ASSERT_EQUAL_UINT16(0, bottom_left.x);
    TEST_ASSERT_EQUAL_UINT16(239, bottom_left.y);
    TEST_ASSERT_EQUAL_UINT16(319, bottom_right.x);
    TEST_ASSERT_EQUAL_UINT16(239, bottom_right.y);
    TEST_ASSERT_EQUAL_UINT16(159, center.x);
    TEST_ASSERT_EQUAL_UINT16(119, center.y);
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

void test_240x320_layout_reserves_header_cards_and_footer(void) {
    UiLayoutMetrics metrics = compute_ui_layout_metrics(240, 320);
    const int cards_bottom = metrics.content_y +
        (2 * metrics.usage_panel_h) + metrics.usage_panel_gap;

    TEST_ASSERT_TRUE(metrics.small_display);
    TEST_ASSERT_EQUAL_INT(48, metrics.logo_size);
    TEST_ASSERT_EQUAL_INT(134, metrics.logo_scale);
    TEST_ASSERT_EQUAL_INT(42, metrics.logo_rendered_width);
    TEST_ASSERT_EQUAL_INT(24, metrics.percentage_font_px);
    TEST_ASSERT_LESS_OR_EQUAL_INT(metrics.footer_y, cards_bottom);
    TEST_ASSERT_LESS_THAN_INT(metrics.screen_height, metrics.page_indicator_y);
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
    TEST_ASSERT_EQUAL_HEX8(0x88, st7789_bgr_madctl(0));
}

void test_320x240_layout_uses_two_horizontal_cards(void) {
    UiLayoutMetrics m = compute_ui_layout_metrics(320, 240);
    TEST_ASSERT_TRUE(m.horizontal_cards);
    TEST_ASSERT_EQUAL_INT(10, m.margin);
    TEST_ASSERT_EQUAL_INT(52, m.content_y);
    TEST_ASSERT_EQUAL_INT(145, m.panel_width);
    TEST_ASSERT_EQUAL_INT(165, m.second_panel_x);
    TEST_ASSERT_EQUAL_INT(126, m.usage_panel_h);
    TEST_ASSERT_EQUAL_INT(196, m.footer_y);
    TEST_ASSERT_EQUAL_INT(227, m.page_indicator_y);
    TEST_ASSERT_LESS_OR_EQUAL_INT(m.page_indicator_y - 18, m.footer_y);
}

void test_240x320_activity_title_clears_logo(void) {
    UiLayoutMetrics m = compute_ui_layout_metrics(240, 320);
    const int transformed_width =
        (LOGO_WIDTH * m.logo_scale + LV_SCALE_NONE - 1) / LV_SCALE_NONE;
    const int logo_right = m.margin + transformed_width;
    const int activity_title_left = (m.screen_width - 127) / 2;
    TEST_ASSERT_EQUAL_INT(m.logo_rendered_width, transformed_width);
    TEST_ASSERT_LESS_OR_EQUAL_INT(activity_title_left, logo_right + 4);
}

void test_320x240_logo_bounds_match_top_left_scaled_image(void) {
    UiLayoutMetrics m = compute_ui_layout_metrics(320, 240);
    const int transformed_width =
        (LOGO_WIDTH * m.logo_scale + LV_SCALE_NONE - 1) / LV_SCALE_NONE;
    const int transformed_height =
        (LOGO_HEIGHT * m.logo_scale + LV_SCALE_NONE - 1) / LV_SCALE_NONE;

    TEST_ASSERT_EQUAL_INT(40, transformed_width);
    TEST_ASSERT_EQUAL_INT(40, transformed_height);
    TEST_ASSERT_EQUAL_INT(50, m.margin + transformed_width);
    TEST_ASSERT_EQUAL_INT(46, 6 + transformed_height);
}

void test_320x240_enterprise_content_fits_card_content_box(void) {
    UiLayoutMetrics m = compute_ui_layout_metrics(320, 240);
    const int useful_height = m.usage_panel_h - 16;

    TEST_ASSERT_LESS_OR_EQUAL_INT(m.usage_bar_y, m.usage_description_y + 14);
    TEST_ASSERT_LESS_OR_EQUAL_INT(m.usage_status_y, m.usage_bar_y + 24);
    TEST_ASSERT_LESS_OR_EQUAL_INT(useful_height, m.usage_status_y + 14);
    TEST_ASSERT_LESS_OR_EQUAL_INT(useful_height, m.usage_reset_y + 14);
}

void test_320x240_pairing_text_clears_status_and_page_dots(void) {
    UiLayoutMetrics m = compute_ui_layout_metrics(320, 240);
    const int title_top = m.content_y + m.pairing_title_y;
    const int instruction_top = m.content_y + m.pairing_instruction_y;
    const int release_top = m.content_y + m.pairing_release_y;
    const int waiting_bottom = m.footer_y + 18;

    TEST_ASSERT_LESS_OR_EQUAL_INT(instruction_top, title_top + 28 + 4);
    TEST_ASSERT_LESS_OR_EQUAL_INT(release_top, instruction_top + 14 + 4);
    TEST_ASSERT_LESS_OR_EQUAL_INT(m.footer_y, release_top + 14 + 4);
    TEST_ASSERT_LESS_OR_EQUAL_INT(m.page_indicator_y, waiting_bottom + 4);
}

void test_240x240_small_fallback_stays_inside_viewport(void) {
    UiLayoutMetrics m = compute_ui_layout_metrics(240, 240);
    const int cards_bottom = m.content_y + m.usage_panel_h;
    const int second_panel_right = m.second_panel_x + m.panel_width;

    TEST_ASSERT_TRUE(m.small_display);
    TEST_ASSERT_TRUE(m.horizontal_cards);
    TEST_ASSERT_LESS_OR_EQUAL_INT(m.screen_width, second_panel_right + m.margin);
    TEST_ASSERT_LESS_OR_EQUAL_INT(m.footer_y, cards_bottom + 18);
    TEST_ASSERT_LESS_OR_EQUAL_INT(m.page_indicator_y, m.footer_y + 18);
    TEST_ASSERT_LESS_OR_EQUAL_INT(m.screen_height, m.page_indicator_y + 5);
}

void test_st7789_landscape_usb_left_mode_uses_bgr_color_order(void) {
    TEST_ASSERT_EQUAL_HEX8(0xE8, st7789_bgr_madctl(3));
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

void test_old_claude_payload_remains_compatible(void) {
    UsageData data{};
    uint8_t mask = parse_dashboard_json(
        "{\"s\":12.5,\"sr\":30,\"w\":34,\"wr\":60,\"ok\":true}",
        &data
    );

    TEST_ASSERT_BITS_HIGH(DASHBOARD_UPDATE_CLAUDE, mask);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 12.5f, data.session_pct);
    TEST_ASSERT_FALSE(data.codex.valid);
    TEST_ASSERT_FALSE(data.activity.valid);
}

void test_new_payload_parses_codex_and_activity(void) {
    UsageData data{};
    uint8_t mask = parse_dashboard_json(
        "{\"v\":2,\"ts\":1000,\"x\":{\"l\":[{\"p\":2,\"wm\":10080,\"rm\":10}],\"td\":120,\"pl\":\"pro\"},\"a\":{\"cl\":{\"o\":3,\"b\":1,\"w\":1},\"cx\":{\"u\":5},\"ts\":1000}}",
        &data
    );

    TEST_ASSERT_BITS_HIGH(DASHBOARD_UPDATE_CODEX, mask);
    TEST_ASSERT_BITS_HIGH(DASHBOARD_UPDATE_ACTIVITY, mask);
    TEST_ASSERT_EQUAL_UINT8(1, data.codex.limit_count);
    TEST_ASSERT_EQUAL_INT(10080, data.codex.limits[0].window_mins);
    TEST_ASSERT_EQUAL_UINT32(120, data.codex.tokens_today);
    TEST_ASSERT_EQUAL_INT(3, data.activity.claude_open);
    TEST_ASSERT_EQUAL_INT(5, data.activity.codex_unread);
}

void test_missing_codex_window_is_not_invented_and_zero_unread_is_valid(void) {
    UsageData data{};
    uint8_t mask = parse_dashboard_json(
        "{\"x\":{\"l\":[],\"td\":0},\"a\":{\"cx\":{\"u\":0},\"ts\":1000}}",
        &data
    );

    TEST_ASSERT_BITS_HIGH(DASHBOARD_UPDATE_CODEX, mask);
    TEST_ASSERT_EQUAL_UINT8(0, data.codex.limit_count);
    TEST_ASSERT_BITS_HIGH(DASHBOARD_UPDATE_ACTIVITY, mask);
    TEST_ASSERT_TRUE(data.activity.codex_valid);
    TEST_ASSERT_EQUAL_INT(0, data.activity.codex_unread);
}

void test_local_only_payload_does_not_update_claude(void) {
    UsageData data = {};
    uint8_t full = parse_dashboard_json("{\"s\":25,\"w\":10,\"v\":2}", &data);
    TEST_ASSERT_BITS_HIGH(DASHBOARD_UPDATE_CLAUDE, full);

    uint8_t local = parse_dashboard_json(
        "{\"v\":2,\"x\":{\"l\":[],\"td\":0},\"a\":{\"cl\":null,\"cx\":{\"u\":0}}}",
        &data
    );
    TEST_ASSERT_BITS_LOW(DASHBOARD_UPDATE_CLAUDE, local);
    TEST_ASSERT_BITS_HIGH(DASHBOARD_UPDATE_CODEX, local);
    TEST_ASSERT_BITS_HIGH(DASHBOARD_UPDATE_ACTIVITY, local);
    TEST_ASSERT_TRUE(data.valid);
}

void test_explicit_tombstones_clear_only_local_providers(void) {
    UsageData data = {};
    parse_dashboard_json(
        "{\"x\":{\"l\":[],\"td\":0},\"a\":{\"cl\":{\"o\":0,\"b\":0,\"w\":0},\"cx\":{\"u\":0}}}",
        &data
    );
    uint8_t mask = parse_dashboard_json(
        "{\"x\":null,\"a\":{\"cl\":null,\"cx\":null}}",
        &data
    );
    TEST_ASSERT_BITS_HIGH(DASHBOARD_UPDATE_CODEX | DASHBOARD_UPDATE_ACTIVITY, mask);
    TEST_ASSERT_FALSE(data.codex.valid);
    TEST_ASSERT_FALSE(data.activity.claude_valid);
    TEST_ASSERT_FALSE(data.activity.codex_valid);
}

void test_activity_freshness_uses_only_activity_updates_and_formats_age(void) {
    UsageData data{};
    ActivityFreshnessState freshness{};
    char footer[32];

    uint8_t mask = parse_dashboard_json(
        "{\"a\":{\"cl\":{\"o\":0,\"b\":0,\"w\":0},\"cx\":{\"u\":0}}}",
        &data
    );
    activity_freshness_apply(freshness, mask, 1000);
    format_activity_freshness(freshness, 1000, footer, sizeof(footer));
    TEST_ASSERT_EQUAL_STRING("Scanned just now", footer);
    TEST_ASSERT_TRUE(data.activity.claude_valid);
    TEST_ASSERT_TRUE(data.activity.codex_valid);
    TEST_ASSERT_EQUAL_INT(0, data.activity.claude_open);
    TEST_ASSERT_EQUAL_INT(0, data.activity.codex_unread);

    mask = parse_dashboard_json("{\"s\":25,\"ok\":true}", &data);
    activity_freshness_apply(freshness, mask, 61000);
    format_activity_freshness(freshness, 61000, footer, sizeof(footer));
    TEST_ASSERT_EQUAL_STRING("Scanned 1m ago", footer);

    mask = parse_dashboard_json("{\"x\":{\"l\":[],\"td\":0}}", &data);
    activity_freshness_apply(freshness, mask, 121000);
    format_activity_freshness(freshness, 121000, footer, sizeof(footer));
    TEST_ASSERT_EQUAL_STRING("Scanned 2m ago", footer);

    mask = parse_dashboard_json("{\"a\":{\"cl\":null,\"cx\":null}}", &data);
    activity_freshness_apply(freshness, mask, 122000);
    format_activity_freshness(freshness, 122000, footer, sizeof(footer));
    TEST_ASSERT_EQUAL_STRING("Scanned just now", footer);
    TEST_ASSERT_FALSE(data.activity.claude_valid);
    TEST_ASSERT_FALSE(data.activity.codex_valid);
}

void test_ui_update_accepts_provider_mask(void) {
    void (*masked_update)(const UsageData*, uint8_t) = ui_update;
    TEST_ASSERT_NOT_NULL(masked_update);
}

void test_codex_window_labels_follow_actual_window_duration(void) {
    TEST_ASSERT_EQUAL_STRING("5 hours", codex_window_label(300));
    TEST_ASSERT_EQUAL_STRING("Weekly", codex_window_label(10080));
    TEST_ASSERT_EQUAL_STRING("Limit", codex_window_label(1440));
}

void test_daily_tokens_are_formatted_compactly(void) {
    char buffer[16];
    format_compact_tokens(12500, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_STRING("12.5k", buffer);
}

void test_carousel_wraps_in_approved_order(void) {
    CarouselState state{};
    carousel_start(state, DASHBOARD_CLAUDE, 1000);
    TEST_ASSERT_EQUAL(DASHBOARD_CODEX, carousel_manual_next(state, 2000));
    TEST_ASSERT_EQUAL(DASHBOARD_ACTIVITY, carousel_manual_next(state, 3000));
    TEST_ASSERT_EQUAL(DASHBOARD_ROBOT, carousel_manual_next(state, 4000));
    TEST_ASSERT_EQUAL(DASHBOARD_CLAUDE, carousel_manual_next(state, 5000));
}

void test_carousel_previous_moves_in_reverse_order(void) {
    CarouselState state{};
    carousel_start(state, DASHBOARD_ACTIVITY, 1000);
    TEST_ASSERT_EQUAL(DASHBOARD_CODEX, carousel_manual_previous(state, 2000));
    TEST_ASSERT_EQUAL(DASHBOARD_CLAUDE, carousel_manual_previous(state, 3000));
    TEST_ASSERT_EQUAL(DASHBOARD_ACTIVITY, carousel_manual_previous(state, 4000));
}

void test_touch_halves_select_previous_and_next(void) {
    TEST_ASSERT_EQUAL(DASHBOARD_NAV_PREVIOUS, dashboard_direction_for_x(0, 320));
    TEST_ASSERT_EQUAL(DASHBOARD_NAV_PREVIOUS, dashboard_direction_for_x(159, 320));
    TEST_ASSERT_EQUAL(DASHBOARD_NAV_NEXT, dashboard_direction_for_x(160, 320));
    TEST_ASSERT_EQUAL(DASHBOARD_NAV_NEXT, dashboard_direction_for_x(319, 320));
    TEST_ASSERT_EQUAL(DASHBOARD_NAV_PREVIOUS, dashboard_direction_for_x(119, 240));
    TEST_ASSERT_EQUAL(DASHBOARD_NAV_NEXT, dashboard_direction_for_x(120, 240));
}

void test_previous_touch_defers_auto_advance_for_thirty_seconds(void) {
    CarouselState state{};
    carousel_start(state, DASHBOARD_CODEX, 1000);
    carousel_manual_previous(state, 5000);
    TEST_ASSERT_FALSE(carousel_tick(state, 34999));
    TEST_ASSERT_TRUE(carousel_tick(state, 35000));
}

void test_carousel_auto_advances_after_twelve_seconds(void) {
    CarouselState state{};
    carousel_start(state, DASHBOARD_CLAUDE, 1000);
    TEST_ASSERT_FALSE(carousel_tick(state, 12999));
    TEST_ASSERT_TRUE(carousel_tick(state, 13000));
    TEST_ASSERT_EQUAL(DASHBOARD_CODEX, state.page);
}

void test_manual_touch_defers_auto_advance_for_thirty_seconds(void) {
    CarouselState state{};
    carousel_start(state, DASHBOARD_CLAUDE, 1000);
    carousel_manual_next(state, 5000);
    TEST_ASSERT_FALSE(carousel_tick(state, 34999));
    TEST_ASSERT_TRUE(carousel_tick(state, 35000));
    TEST_ASSERT_EQUAL(DASHBOARD_ACTIVITY, state.page);
}

void setup() {
    delay(2000);
    UNITY_BEGIN();
    RUN_TEST(test_touch_mapping_keeps_portrait_axes);
    RUN_TEST(test_touch_mapping_clamps_edges);
    RUN_TEST(test_landscape_touch_mapping_rotates_with_usb_left);
    RUN_TEST(test_short_press_emits_only_short_event);
    RUN_TEST(test_long_press_does_not_emit_short_event);
    RUN_TEST(test_240x320_layout_reserves_header_cards_and_footer);
    RUN_TEST(test_320x240_layout_uses_two_horizontal_cards);
    RUN_TEST(test_240x320_activity_title_clears_logo);
    RUN_TEST(test_320x240_logo_bounds_match_top_left_scaled_image);
    RUN_TEST(test_320x240_enterprise_content_fits_card_content_box);
    RUN_TEST(test_320x240_pairing_text_clears_status_and_page_dots);
    RUN_TEST(test_240x240_small_fallback_stays_inside_viewport);
    RUN_TEST(test_existing_layout_breakpoints_remain_distinct);
    RUN_TEST(test_small_no_psram_splash_keeps_heap_headroom);
    RUN_TEST(test_serial_protocol_recognizes_usage_json);
    RUN_TEST(test_serial_protocol_recognizes_identify_command);
    RUN_TEST(test_st7789_portrait_mode_uses_bgr_color_order);
    RUN_TEST(test_st7789_landscape_usb_left_mode_uses_bgr_color_order);
    RUN_TEST(test_fresh_serial_data_selects_live_usage_without_ble);
    RUN_TEST(test_stale_data_without_ble_selects_waiting_view);
    RUN_TEST(test_old_claude_payload_remains_compatible);
    RUN_TEST(test_new_payload_parses_codex_and_activity);
    RUN_TEST(test_missing_codex_window_is_not_invented_and_zero_unread_is_valid);
    RUN_TEST(test_local_only_payload_does_not_update_claude);
    RUN_TEST(test_explicit_tombstones_clear_only_local_providers);
    RUN_TEST(test_activity_freshness_uses_only_activity_updates_and_formats_age);
    RUN_TEST(test_ui_update_accepts_provider_mask);
    RUN_TEST(test_codex_window_labels_follow_actual_window_duration);
    RUN_TEST(test_daily_tokens_are_formatted_compactly);
    RUN_TEST(test_carousel_wraps_in_approved_order);
    RUN_TEST(test_carousel_previous_moves_in_reverse_order);
    RUN_TEST(test_touch_halves_select_previous_and_next);
    RUN_TEST(test_previous_touch_defers_auto_advance_for_thirty_seconds);
    RUN_TEST(test_carousel_auto_advances_after_twelve_seconds);
    RUN_TEST(test_manual_touch_defers_auto_advance_for_thirty_seconds);
    UNITY_END();
}

void loop() {}
