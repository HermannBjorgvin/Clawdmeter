#pragma once
#include "data.h"
#include "ble.h"

// The default (splash) screen morphs to show Activity content when the
// daemon reports active sessions, and reverts to the Clawd animation
// when all sessions have gone idle. Activity is therefore not a
// separately cycled screen — it lives inside SCREEN_SPLASH.
enum screen_t {
    SCREEN_SPLASH,
    SCREEN_USAGE,
    SCREEN_BLUETOOTH,
    SCREEN_COUNT,
};

void ui_init(void);
void ui_update(const UsageData* data);
void ui_update_activity(const ActivityData* data);
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
void ui_cycle_screen(void);
void ui_toggle_splash(void);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);

// Mute indicator on the Activity title row. Pass `visible=true` to render
// a small "MUTE" tag; pass `false` to hide. Cheap — safe to call from
// any context; idempotent.
void ui_set_mute_indicator(bool visible);

// Brief on-screen toast (1.5s) confirming a mute toggle. Renders over
// whatever screen is currently active.
void ui_flash_mute_toast(bool now_muted);
