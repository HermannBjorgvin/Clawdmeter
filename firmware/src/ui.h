#pragma once
#include "data.h"
#include "ble.h"

// SCREEN_SPLASH is the boot screen / screensaver art; it is never part of the
// mode ring. The four modes after it cycle FOCUS -> CHATS -> CLASSIC -> RINGS
// -> FOCUS on a tap (or a physical top button) and the active one is
// remembered in NVS across reboots.
enum screen_t {
    SCREEN_SPLASH,
    SCREEN_FOCUS,     // one-chat glance: two slim limit rows + big context card
    SCREEN_CHATS,     // every active chat on one screen
    SCREEN_CLASSIC,   // the original two big usage panels + context strip
    SCREEN_RINGS,     // watch face: concentric ctx / 5h / 7d rings
    SCREEN_COUNT,
};
#define SCREEN_MODE_FIRST SCREEN_FOCUS
#define SCREEN_MODE_LAST  SCREEN_RINGS

void ui_init(void);
void ui_update(const UsageData* data);
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);

// Step through the mode ring (never lands on the splash). From the splash,
// either direction returns to the remembered mode.
void ui_mode_next(void);
void ui_mode_prev(void);

screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);
