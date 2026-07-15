#pragma once
#include "data.h"
#include "ble.h"

enum screen_t {
    SCREEN_SPLASH,
    SCREEN_SYSTEM,  // host CPU/GPU/RAM — immediately left of Claude
    SCREEN_USAGE,   // Claude tab
    SCREEN_CODEX,   // Codex tab — swipe left from SCREEN_USAGE
    SCREEN_ANTIGRAVITY, // Antigravity CLI tab — swipe left from SCREEN_CODEX
    SCREEN_STATS,   // /stats view — tap the title on any provider tab
    SCREEN_COUNT,
};

void ui_init(void);
void ui_update(const UsageData* data);
void ui_update_stats(const StatsData* claude, const StatsData* codex,
                     const StatsData* antigravity);
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
void ui_toggle_splash(void);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);
