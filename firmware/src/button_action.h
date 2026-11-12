#pragma once
#include <stdint.h>

// What a physical button does when pressed: device settings `k1`, `k2` and
// `k3`, the PRIMARY, SECONDARY and PWR buttons (PWR's hold to pair is fixed).
// Pure: no Arduino, so it runs in the native unit tests. The values are the
// wire format; never renumber them.
//
//   0..6      a preset (enum below)
//   0x10000 | hold << 12 | mods << 8 | usage
//             a keyboard chord: `usage` is a HID keyboard usage
//             (BTN_KEY_USAGE_MIN..BTN_KEY_USAGE_MAX), `mods` the HID modifier
//             bits Ctrl 1, Shift 2, Alt 4, GUI 8 (left-hand keys), `hold` 1 =
//             keep the keys down while the button is held, 0 = tap them
//   0x20000 | usage
//             a HID Consumer Control usage (1..BTN_CONSUMER_MAX): media keys;
//             volume up/down repeat while the button is held, others tap
//
// Anything else (bits outside these fields, usage 0 or out of range) is
// invalid: the payload parser ignores it (payload.cpp) and a stored one
// reads as that button's default.
enum ButtonPreset {
    BTN_ACT_NONE       = 0,   // nothing (a press still wakes the screen and dismisses an alert)
    BTN_ACT_SPACE      = 1,   // HID Space, held: Claude Code voice-mode push to talk (primary default)
    BTN_ACT_SHIFT_TAB  = 2,   // HID Shift+Tab: mode toggle (secondary default)
    BTN_ACT_TAB        = 3,   // HID Tab
    BTN_ACT_ENTER      = 4,   // HID Enter
    BTN_ACT_ESCAPE     = 5,   // HID Escape
    BTN_ACT_BRIGHTNESS = 6,   // cycle the brightness (PWR default)
    BTN_ACT_PRESET_COUNT
};

#define BTN_ACT_PRIMARY_DEFAULT   BTN_ACT_SPACE
#define BTN_ACT_SECONDARY_DEFAULT BTN_ACT_SHIFT_TAB
#define BTN_ACT_PWR_DEFAULT       BTN_ACT_BRIGHTNESS

#define BTN_ACT_KEY_FLAG      0x10000
#define BTN_ACT_CONSUMER_FLAG 0x20000
#define BTN_ACT_HOLD_BIT      0x1000
// The keyboard report's usage range (ble.cpp HID_REPORT_MAP): A..F24.
#define BTN_KEY_USAGE_MIN     0x04
#define BTN_KEY_USAGE_MAX     0x73
// The consumer report's usage range (ble.cpp HID_REPORT_MAP).
#define BTN_CONSUMER_MAX      0x3FF

// HID Consumer usages the app offers.
#define HID_CONSUMER_PLAY_PAUSE 0x00CD
#define HID_CONSUMER_NEXT       0x00B5
#define HID_CONSUMER_PREVIOUS   0x00B6
#define HID_CONSUMER_MUTE       0x00E2
#define HID_CONSUMER_VOL_UP     0x00E9
#define HID_CONSUMER_VOL_DOWN   0x00EA

enum ButtonKind {
    BTN_KIND_NONE,
    BTN_KIND_KEY,          // keyboard report: `key` + `modifiers`
    BTN_KIND_CONSUMER,     // consumer report: `consumer`
    BTN_KIND_BRIGHTNESS,
};

struct ButtonBinding {
    uint8_t  kind;         // ButtonKind
    uint8_t  key;          // HID keyboard usage
    uint8_t  modifiers;    // HID modifier byte
    uint16_t consumer;     // HID consumer usage
    bool     hold;         // keys stay down while the button is held
    bool     repeat;       // re-tapped while the button is held (volume)
};

// Whether `v` is a value this firmware understands.
bool button_action_valid(int32_t v);

// `v` when valid, else `fallback`.
int32_t button_action_clamp(int32_t v, int32_t fallback);

// What a valid value does. An invalid one decodes as BTN_KIND_NONE.
ButtonBinding button_action_decode(int32_t v);

// The keyboard report while buttons hold keys down. Each holder (a button)
// has its own modifiers and key; the report is all of them merged with an
// optional tapped chord, so one button's tap or release never lifts keys
// another button is holding.
#define KB_HOLDERS 3
struct KeyboardHeld {
    uint8_t mods[KB_HOLDERS];
    uint8_t keys[KB_HOLDERS];
};

// The 8-byte report: [modifiers, 0, key1..key6]. `extra_*` is a chord being
// tapped on top (0 for none).
void keyboard_report(const KeyboardHeld* held, uint8_t extra_mods, uint8_t extra_key, uint8_t out[8]);

// Volume repeat timing while a button is held.
#define BTN_REPEAT_DELAY_MS    500
#define BTN_REPEAT_INTERVAL_MS 120
