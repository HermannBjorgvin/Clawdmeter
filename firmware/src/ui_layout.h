#pragma once

#include <stdint.h>

struct UiLayoutMetrics {
    int16_t screen_width;
    int16_t screen_height;
    int16_t margin;
    int16_t title_y;
    int16_t content_y;
    int16_t content_width;
    int16_t panel_width;
    int16_t second_panel_x;
    int16_t usage_panel_h;
    int16_t usage_panel_gap;
    int16_t usage_bar_y;
    int16_t usage_reset_y;
    int16_t logo_size;
    int16_t logo_scale;
    int16_t logo_rendered_width;
    int16_t percentage_font_px;
    int16_t footer_y;
    int16_t page_indicator_y;
    int16_t bluetooth_panel_h;
    int16_t bluetooth_reset_zone_h;
    bool small_display;
    bool horizontal_cards;
    int16_t status_font_px;
    int16_t idle_creature_size;
};

inline UiLayoutMetrics compute_ui_layout_metrics(
    int16_t width,
    int16_t height
) {
    UiLayoutMetrics metrics{};
    metrics.screen_width = width;
    metrics.screen_height = height;
    metrics.margin = 20;
    metrics.title_y = 30;
    metrics.logo_size = 80;
    metrics.logo_scale = 256;
    metrics.logo_rendered_width = 80;
    metrics.percentage_font_px = 48;
    metrics.footer_y = height - 45;
    metrics.page_indicator_y = height - 16;
    metrics.small_display = false;
    metrics.status_font_px = 32;
    metrics.idle_creature_size = 160;

    if (width == 320 && height == 240) {
        metrics.margin = 10;
        metrics.title_y = 8;
        metrics.content_y = 52;
        metrics.panel_width = 145;
        metrics.second_panel_x = 165;
        metrics.usage_panel_h = 126;
        metrics.usage_panel_gap = 10;
        metrics.usage_bar_y = 68;
        metrics.usage_reset_y = 92;
        metrics.logo_size = 40;
        metrics.logo_scale = 128;
        metrics.logo_rendered_width = 40;
        metrics.percentage_font_px = 24;
        metrics.footer_y = 196;
        metrics.page_indicator_y = 227;
        metrics.bluetooth_panel_h = 92;
        metrics.bluetooth_reset_zone_h = 65;
        metrics.small_display = true;
        metrics.horizontal_cards = true;
        metrics.status_font_px = 14;
        metrics.idle_creature_size = 92;
    } else if (width == 240 && height == 320) {
        metrics.margin = 10;
        metrics.title_y = 12;
        metrics.content_y = 64;
        metrics.usage_panel_h = 88;
        metrics.usage_panel_gap = 8;
        metrics.usage_bar_y = 36;
        metrics.usage_reset_y = 61;
        metrics.logo_size = 48;
        metrics.logo_scale = 134;
        metrics.logo_rendered_width = 42;
        metrics.percentage_font_px = 24;
        metrics.footer_y = 276;
        metrics.page_indicator_y = 304;
        metrics.bluetooth_panel_h = 92;
        metrics.bluetooth_reset_zone_h = 65;
        metrics.small_display = true;
        metrics.status_font_px = 14;
        metrics.idle_creature_size = 92;
    } else if (height >= 460) {
        metrics.content_y = 100;
        metrics.usage_panel_h = 150;
        metrics.usage_panel_gap = 16;
        metrics.usage_bar_y = 56;
        metrics.usage_reset_y = 94;
        metrics.bluetooth_panel_h = 160;
        metrics.bluetooth_reset_zone_h = 110;
    } else {
        metrics.content_y = 85;
        metrics.usage_panel_h = 130;
        metrics.usage_panel_gap = 12;
        metrics.usage_bar_y = 48;
        metrics.usage_reset_y = 78;
        metrics.bluetooth_panel_h = 140;
        metrics.bluetooth_reset_zone_h = 90;
    }

    metrics.content_width = width - (2 * metrics.margin);
    if (!metrics.horizontal_cards) {
        metrics.panel_width = metrics.content_width;
        metrics.second_panel_x = metrics.margin;
    }
    return metrics;
}
