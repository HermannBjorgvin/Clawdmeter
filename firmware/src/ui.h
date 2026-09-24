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
void ui_update_battery(int percent, bool charging);

// Re-apply the active theme's colors to the already-built usage screen.
// Call after theme_set_mode(). Bar fills and pace colors are recomputed on
// every ui_update() and so track the theme on their own; this covers the
// tokens that are only applied at build time.
void ui_apply_theme(void);

// Briefly replace the status line with a transient message, then let the
// ticker resume. Used to confirm a button gesture that has no other visible
// effect at the moment it fires.
void ui_flash_hint(const char* text, uint32_t ms);
