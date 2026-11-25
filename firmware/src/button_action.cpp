#include "button_action.h"

bool button_action_valid(int32_t v) {
    if (v >= 0 && v < BTN_ACT_PRESET_COUNT) return true;
    if ((v & ~0x1FFF) == BTN_ACT_KEY_FLAG) {
        const int usage = v & 0xFF;
        return usage >= BTN_KEY_USAGE_MIN && usage <= BTN_KEY_USAGE_MAX;
    }
    if ((v & ~0xFFF) == BTN_ACT_CONSUMER_FLAG) {
        const int usage = v & 0xFFF;
        return usage >= 1 && usage <= BTN_CONSUMER_MAX;
    }
    return false;
}

int32_t button_action_clamp(int32_t v, int32_t fallback) {
    return button_action_valid(v) ? v : fallback;
}

static ButtonBinding key(uint8_t usage, uint8_t mods, bool hold) {
    ButtonBinding b = {};
    b.kind = BTN_KIND_KEY;
    b.key = usage;
    b.modifiers = mods;
    b.hold = hold;
    return b;
}

ButtonBinding button_action_decode(int32_t v) {
    ButtonBinding b = {};
    if (!button_action_valid(v)) return b;
    switch (v) {
        case BTN_ACT_NONE:       return b;
        case BTN_ACT_SPACE:      return key(0x2C, 0x00, true);
        case BTN_ACT_SHIFT_TAB:  return key(0x2B, 0x02, false);   // LEFT_SHIFT
        case BTN_ACT_TAB:        return key(0x2B, 0x00, false);
        case BTN_ACT_ENTER:      return key(0x28, 0x00, false);
        case BTN_ACT_ESCAPE:     return key(0x29, 0x00, false);
        case BTN_ACT_BRIGHTNESS: b.kind = BTN_KIND_BRIGHTNESS; return b;
        default: break;
    }
    if (v & BTN_ACT_KEY_FLAG) {
        return key((uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0x0F), (v & BTN_ACT_HOLD_BIT) != 0);
    }
    b.kind = BTN_KIND_CONSUMER;
    b.consumer = (uint16_t)(v & 0xFFF);
    b.repeat = b.consumer == HID_CONSUMER_VOL_UP || b.consumer == HID_CONSUMER_VOL_DOWN;
    return b;
}

void keyboard_report(const KeyboardHeld* held, uint8_t extra_mods, uint8_t extra_key, uint8_t out[8]) {
    for (int i = 0; i < 8; i++) out[i] = 0;
    int n = 2;
    out[0] = extra_mods;
    for (int h = 0; h < KB_HOLDERS; h++) {
        out[0] |= held->mods[h];
        const uint8_t k = held->keys[h];
        bool dup = false;
        for (int j = 2; j < n; j++) dup = dup || out[j] == k;
        if (k && !dup && n < 8) out[n++] = k;
    }
    bool dup = false;
    for (int j = 2; j < n; j++) dup = dup || out[j] == extra_key;
    if (extra_key && !dup && n < 8) out[n++] = extra_key;
}
