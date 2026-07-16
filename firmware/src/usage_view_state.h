#pragma once

#include <stdint.h>

enum UsageViewState {
    USAGE_VIEW_WAITING = 0,
    USAGE_VIEW_IDLE = 1,
    USAGE_VIEW_LIVE = 2,
};

inline UsageViewState select_usage_view_state(
    bool ble_connected,
    bool data_received,
    uint32_t now_ms,
    uint32_t last_data_ms,
    uint32_t fresh_ms
) {
    if (data_received && (now_ms - last_data_ms) < fresh_ms) {
        return USAGE_VIEW_LIVE;
    }
    return ble_connected ? USAGE_VIEW_IDLE : USAGE_VIEW_WAITING;
}
