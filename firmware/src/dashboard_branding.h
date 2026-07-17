#pragma once

#include <stdint.h>

#include "dashboard_carousel.h"

enum DashboardBrandMask : uint8_t {
    DASHBOARD_BRAND_NONE = 0,
    DASHBOARD_BRAND_CLAUDE = 1,
    DASHBOARD_BRAND_CODEX = 2,
};

struct DashboardVisibilityState {
    bool dashboard_active;
};

inline void dashboard_visibility_show_boot_splash(DashboardVisibilityState& state) {
    state.dashboard_active = false;
}

inline void dashboard_visibility_show_dashboard(DashboardVisibilityState& state) {
    state.dashboard_active = true;
}

inline uint8_t dashboard_brand_mask(DashboardPage page) {
    if (page == DASHBOARD_CLAUDE) return DASHBOARD_BRAND_CLAUDE;
    if (page == DASHBOARD_CODEX) return DASHBOARD_BRAND_CODEX;
    if (page == DASHBOARD_ACTIVITY) {
        return DASHBOARD_BRAND_CLAUDE | DASHBOARD_BRAND_CODEX;
    }
    return DASHBOARD_BRAND_NONE;
}

inline bool dashboard_battery_visible(
    const DashboardVisibilityState& state,
    DashboardPage page
) {
    return state.dashboard_active && page != DASHBOARD_ACTIVITY;
}
