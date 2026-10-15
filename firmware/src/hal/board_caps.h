#pragma once
#include <stdint.h>

// Runtime board description consumed by board-agnostic code (UI, main loop).
// Each board provides a single BoardCaps instance via board_caps().
//
// Compile-time-only facts (pin numbers, library choice) belong in
// boards/<name>/board.h and never leak into shared code. Anything the UI or
// main loop needs at runtime — display size, optional-feature presence —
// goes here so shared code stays free of #ifdef BOARD_*.
struct BoardCaps {
    const char* name;        // human-readable, e.g. "Waveshare AMOLED 2.16"

    int16_t width;           // active display width in pixels
    int16_t height;          // active display height in pixels
    bool    round_panel;     // the visible panel is a circle inscribed in
                             // width x height (round AMOLEDs); the meters
                             // keep inside it (layout.h)

    uint8_t button_count;    // 0 = no primary/secondary: the PWR-role key is the
                             // only button (or there is none); 1 = primary
                             // (BOOT) only; 2 = primary + secondary
    bool    has_rotation;    // IMU-driven CPU rotation in the flush callback
    bool    has_battery;     // AXP2101 battery measurement is meaningful
    bool    has_imu;         // QMI8658 (or compatible) is populated
    bool    minimal_ui;      // minimal mode: meters + a still corner Clawd,
                             // nothing else animates (e-ink, slow panels).
                             // Shared code reads it through minimal_mode().
    bool    has_pwr_button;  // a PWR-role button reaches the firmware (AXP PKEY,
                             // expander pin or GPIO): brightness and
                             // hold-to-pair. Without one (LCD-4: KEY/PWR is a
                             // hardware reset) a 5 s touch hold on the pairing
                             // hint clears the bonds instead (pair_hold.h).
    bool    has_touch;       // a touchscreen. Without one BOOT (the only
                             // button when button_count is 0) dismisses an
                             // active alert before it does anything else
                             // (director_button).
};

const BoardCaps& board_caps(void);
