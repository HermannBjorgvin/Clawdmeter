#pragma once
#include "data.h"
#include "ble.h"

enum screen_t {
    SCREEN_SPLASH,
    SCREEN_USAGE,     // overview: session + weekly bars
    SCREEN_TREND,     // sparkline history of session & weekly
    SCREEN_BURN,      // burn rate (%/hr) + projected time-to-100%
    SCREEN_SESSION,   // session (5h) detail
    SCREEN_WEEKLY,    // weekly (7d) detail
    SCREEN_COUNT,
    // First and last of the cyclable (non-splash) pages, for ui_next_page().
    SCREEN_PAGE_FIRST = SCREEN_USAGE,
    SCREEN_PAGE_LAST  = SCREEN_WEEKLY,
};

void ui_init(void);
void ui_update(const UsageData* data);
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
void ui_next_page(void);   // advance to the next cyclable page (wraps)
void ui_prev_page(void);   // go to the previous cyclable page (wraps)
void ui_note_swipe(void);  // record that a swipe just fired (suppresses the tap→splash toggle)
void ui_toggle_splash(void);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);   // battery glyph (left of the flame) when a cell is present
void ui_flame_tick(void);   // per-frame animation of the flame corner indicator (call each loop)
void ui_cycle_costume(int dir);   // swipe up (+1) / down (-1): change Claude's costume
