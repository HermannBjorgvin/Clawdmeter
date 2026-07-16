#pragma once

#include <stdint.h>
#include "data.h"

enum DashboardUpdateMask : uint8_t {
    DASHBOARD_UPDATE_NONE = 0,
    DASHBOARD_UPDATE_CLAUDE = 1,
    DASHBOARD_UPDATE_CODEX = 2,
    DASHBOARD_UPDATE_ACTIVITY = 4,
};

uint8_t parse_dashboard_json(const char* json, UsageData* out);
