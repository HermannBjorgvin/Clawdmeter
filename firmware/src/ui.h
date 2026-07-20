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
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
void ui_toggle_splash(void);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
// Show the attention view: wakes the display, jumps to the usage screen and
// overlays a creature + caption matching the event type (1 = waiting for
// input, 2 = permission needed, 3 = work done); the header shows `project`
// (may be "" for none) so it's clear which session wants you. Dismissed by a
// tap or automatically after a timeout. ui_hide_attention() dismisses it
// remotely (e.g. the user started typing on the host again).
void ui_show_attention(uint8_t type, const char* project);
void ui_hide_attention(void);
// Daemon error beat ("ok":false): force the idle view now (don't render stale
// numbers as live) and show the reason in the status line. Codes: auth /
// token / rate / net / http. Cleared automatically by the next good payload.
void ui_set_data_error(const char* code);
void ui_update_battery(int percent, bool charging);
