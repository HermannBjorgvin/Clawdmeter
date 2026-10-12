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
void ui_update_stats(const StatsData* stats);
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
void ui_toggle_splash(void);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging, bool usb_power);  // percent < 0 = no battery
// Attention: mascot walk-on plus the message on the status line. Ends on a
// tap, after 2 minutes, or ui_hide_alert() when the session moves on.
void ui_show_alert(const char* message);
void ui_hide_alert(void);
