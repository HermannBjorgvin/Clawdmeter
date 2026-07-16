#pragma once
#include "data.h"
#include "ble.h"
#include "dashboard_carousel.h"

void ui_init(void);
void ui_update(const UsageData* data);
void ui_tick_anim(void);
void ui_show_screen(DashboardPage page);
void ui_start_dashboard(uint32_t now_ms);
void ui_tick_navigation(uint32_t now_ms);
DashboardPage ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);
