#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "dashboard_payload.h"

struct ActivityFreshnessState {
    uint32_t last_scan_ms;
    bool has_scan;
};

inline void activity_freshness_apply(
    ActivityFreshnessState& state,
    uint8_t updates,
    uint32_t now_ms
) {
    if (!(updates & DASHBOARD_UPDATE_ACTIVITY)) return;
    state.last_scan_ms = now_ms;
    state.has_scan = true;
}

inline void format_activity_freshness(
    const ActivityFreshnessState& state,
    uint32_t now_ms,
    char* buffer,
    size_t length
) {
    if (!buffer || length == 0) return;
    if (!state.has_scan) {
        snprintf(buffer, length, "Not scanned");
        return;
    }

    const uint32_t age_minutes = (now_ms - state.last_scan_ms) / 60000U;
    if (age_minutes == 0) {
        snprintf(buffer, length, "Scanned just now");
    } else {
        snprintf(
            buffer,
            length,
            "Scanned %lum ago",
            static_cast<unsigned long>(age_minutes)
        );
    }
}
