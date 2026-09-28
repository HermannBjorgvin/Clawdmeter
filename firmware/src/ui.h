#pragma once
#include "data.h"
#include "ble.h"

enum screen_t {
    SCREEN_SPLASH,
    SCREEN_USAGE,
    SCREEN_COUNT,
};

void ui_init(void);
void ui_update(const UsageData* data);
// What changed that deserves attention. For either, the UI has already jumped
// to the Agents page; the caller wakes the panel (and chimes for WAITING).
enum agents_nudge_t { AGENTS_NUDGE_NONE, AGENTS_NUDGE_DONE, AGENTS_NUDGE_WAITING };
agents_nudge_t ui_update_agents(const AgentsData* data);
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
void ui_toggle_splash(void);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);
