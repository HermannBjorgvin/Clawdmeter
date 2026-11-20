#pragma once
// The firmware-update screen: a heading, the percentage, a bar and a note,
// over everything else (lv_layer_top). No Clawd: whatever he was doing
// carries on underneath, out of sight. Sized from the meters layout, so it
// fits every panel the meters do, round ones included.
#include <stdint.h>

enum OtaUiPhase {
    OTA_UI_CONFIRM,      // asking to allow it: `pct` is the seconds left
    OTA_UI_RECEIVING,    // data arriving: the bar follows `pct`
    OTA_UI_VERIFYING,    // every byte in, checking the image
    OTA_UI_RESTARTING,   // verified, about to restart into it
    OTA_UI_FAILED,       // stopped; ota.cpp hides it a few seconds later
};

void ota_ui_show(OtaUiPhase phase, int pct);   // pct 0..100 (seconds left for CONFIRM)
void ota_ui_hide(void);
bool ota_ui_visible(void);
// The screen was tapped while it asked to allow an update (once per tap).
bool ota_ui_take_tap(void);
