#pragma once
#include "data.h"
#include "ble.h"

// The meters room: clock, the two usage cards, the status line, battery and
// the corner Clawd's seat. Rooms, transitions and attention sequencing belong
// to the director (clawd_director.h); ui only draws the meters.

void ui_init(void);
void ui_update(const UsageData* data);
void ui_tick_anim(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging, bool usb_power);  // percent < 0 = no battery

// True while the meters show live numbers (connected, data fresh) rather than
// the pairing hint or the stale-data idle view.
bool ui_meters_live(void);

// The pairing hint (shown while disconnected) is on screen.
bool ui_pair_hint_visible(void);

enum PairHint {
    PAIR_HINT_NORMAL,    // how to pair on this board
    PAIR_HINT_HOLDING,   // a touch hold toward clearing the pairing is running
    PAIR_HINT_CLEARED,   // bonds were just cleared: ready to pair (reverts by itself)
};
void ui_set_pair_hint(PairHint hint);

// Attention message on the status line (scrolls when long); NULL clears it
// and the whimsical status line resumes.
void ui_set_alert_text(const char* message);
