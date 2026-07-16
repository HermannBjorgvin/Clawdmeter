#pragma once

#include <stdint.h>

constexpr uint32_t DASHBOARD_AUTO_MS = 12000;
constexpr uint32_t DASHBOARD_MANUAL_HOLDOFF_MS = 30000;

enum DashboardPage : uint8_t {
    DASHBOARD_CLAUDE,
    DASHBOARD_CODEX,
    DASHBOARD_ACTIVITY,
    DASHBOARD_ROBOT,
    DASHBOARD_PAGE_COUNT,
};

struct CarouselState {
    DashboardPage page;
    uint32_t next_advance_ms;
    bool started;
};

inline bool dashboard_time_reached(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}

inline DashboardPage dashboard_next_page(DashboardPage page) {
    return static_cast<DashboardPage>((static_cast<uint8_t>(page) + 1) % DASHBOARD_PAGE_COUNT);
}

inline void carousel_start(CarouselState& state, DashboardPage page, uint32_t now) {
    state.page = page;
    state.next_advance_ms = now + DASHBOARD_AUTO_MS;
    state.started = true;
}

inline DashboardPage carousel_manual_next(CarouselState& state, uint32_t now) {
    state.page = dashboard_next_page(state.page);
    state.next_advance_ms = now + DASHBOARD_MANUAL_HOLDOFF_MS;
    state.started = true;
    return state.page;
}

inline bool carousel_tick(CarouselState& state, uint32_t now) {
    if (!state.started || !dashboard_time_reached(now, state.next_advance_ms)) return false;
    state.page = dashboard_next_page(state.page);
    state.next_advance_ms = now + DASHBOARD_AUTO_MS;
    return true;
}
