#pragma once
#include "data.h"
#include "ble.h"

enum screen_t {
    SCREEN_SPLASH,
    SCREEN_USAGE,     // overview: session + weekly bars
    SCREEN_TREND,     // sparkline history of session & weekly
    SCREEN_BURN,      // burn rate (%/hr) + projected time-to-100%
    SCREEN_RHYTHM,    // last-24h activity (session burn per time bucket)
    SCREEN_RECORDS,   // peak %, peak burn, resets survived
    SCREEN_SYSTEM,    // device diagnostics + settings (kiosk-cycle toggle); auto-cycle skips it
    SCREEN_COUNT,
    // First and last of the cyclable (non-splash) pages, for ui_next_page().
    SCREEN_PAGE_FIRST = SCREEN_USAGE,
    SCREEN_PAGE_LAST  = SCREEN_SYSTEM,
};

void ui_init(void);
void ui_update(const UsageData* data);
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
void ui_next_page(void);   // advance to the next cyclable page (wraps)
void ui_prev_page(void);   // go to the previous cyclable page (wraps)
void ui_note_swipe(void);  // record that a swipe just fired (suppresses the tap→splash toggle)
void ui_note_touch(bool down);  // raw touch state from the input layer (interrupts the kiosk)
void ui_apply_pending_gestures(void);  // apply a background-recognised swipe (call from loop)
void ui_toggle_splash(void);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);   // battery glyph (left of the flame) when a cell is present
void ui_flame_tick(void);   // per-frame animation of the flame corner indicator (call each loop)
void ui_cycle_costume(int dir);   // swipe up (+1) / down (-1): change Claude's costume
void ui_trend_refresh(void);      // re-render the Trend page (e.g. after sim data is loaded)
void ui_trend_zoom_step(int dir); // dev: step the Trend zoom window (+1 wider / -1 narrower)
void ui_kiosk_tick(void);         // kiosk auto-cycle timer (call each loop); skips System, resets on manual swipe
void ui_system_tick(void);        // refresh System-page stats (~1/s) while it's visible
void ui_debug_quip(void);         // dev: pop a context-aware mascot quip (for QA / the `quip` cmd)
void ui_debug_cycle_chatty(void); // dev: cycle Clawd's chatter mode (the `chatty` cmd)
