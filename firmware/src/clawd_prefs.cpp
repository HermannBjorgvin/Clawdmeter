#include "clawd_prefs.h"
#include "clawd_director.h"
#include "clawd_policy.h"
#include "ui.h"
#include "face.h"
#include "clawd_anim.h"
#include "mascot.h"
#include "performer.h"
#include "button_action.h"
#include "hal/board_caps.h"
#include <Preferences.h>
#include <Arduino.h>

// Per ButtonSlot: its NVS key and default (`k1`..`k3`).
static const char* const BTN_KEYS[BTN_SLOT_COUNT] = { "btn_k1", "btn_k2", "btn_k3" };
static const int32_t BTN_DEFAULTS[BTN_SLOT_COUNT] = {
    BTN_ACT_PRIMARY_DEFAULT, BTN_ACT_SECONDARY_DEFAULT, BTN_ACT_PWR_DEFAULT,
};
static int32_t btn_actions[BTN_SLOT_COUNT] = {
    BTN_ACT_PRIMARY_DEFAULT, BTN_ACT_SECONDARY_DEFAULT, BTN_ACT_PWR_DEFAULT,
};

void clawd_prefs_init(void) {
    Preferences prefs;
    prefs.begin("clawdmeter", true);
    const int lively = prefs.getUChar("lively", CLAWD_LIVELINESS_DEFAULT);
    const int attn = prefs.getUChar("attn_anim", CLAWD_ATTN_ANIM_DEFAULT_S);
    const bool pace = prefs.getUChar("pace_mark", 0) != 0;
    for (int i = 0; i < BTN_SLOT_COUNT; i++)
        btn_actions[i] = button_action_clamp(prefs.getInt(BTN_KEYS[i], BTN_DEFAULTS[i]), BTN_DEFAULTS[i]);
    prefs.end();
    director_set_liveliness(clawd_liveliness_clamp(lively));
    director_set_attn_anim(clawd_attn_anim_clamp(attn));
    pace_marker_set(pace);
    Serial.printf("Device prefs init: liveliness %d, attention animation %d s, pace marker %s, "
                  "buttons 0x%lx 0x%lx 0x%lx\n",
                  clawd_liveliness(), clawd_attn_anim(), pace ? "on" : "off",
                  (unsigned long)btn_actions[0], (unsigned long)btn_actions[1], (unsigned long)btn_actions[2]);
}

// The palette is kept as its `th` hex string.
void theme_prefs_load(void) {
    if (board_caps().mono_panel) return;   // 1-bit: the default palette only
    Preferences prefs;
    prefs.begin("clawdmeter", true);
    char hex[THEME_HEX_LEN + 1] = {};
    const size_t n = prefs.isKey("theme") ? prefs.getBytes("theme", hex, THEME_HEX_LEN) : 0;
    prefs.end();
    uint32_t p[TH_COUNT];
    if (n == THEME_HEX_LEN && theme_parse_hex(hex, p)) {
        theme_palette_set(p);
        clawd_anim_recolor();
        Serial.printf("Theme: %s (saved)\n", hex);
    }
}

static void save(const char* key, uint8_t v) {
    Preferences prefs;
    prefs.begin("clawdmeter", false);
    prefs.putUChar(key, v);
    prefs.end();
}

void liveliness_set(int level) {
    level = clawd_liveliness_clamp(level);
    if (level == clawd_liveliness()) return;
    save("lively", (uint8_t)level);
    director_set_liveliness(level);
    Serial.printf("Liveliness: %d\n", level);
}

int liveliness_get(void) { return clawd_liveliness(); }

void attn_anim_set(int seconds) {
    seconds = clawd_attn_anim_clamp(seconds);
    if (seconds == clawd_attn_anim()) return;
    save("attn_anim", (uint8_t)seconds);
    director_set_attn_anim(seconds);
    Serial.printf("Attention animation: %d s\n", seconds);
}

int attn_anim_get(void) { return clawd_attn_anim(); }

static bool pace_marker = false;

void pace_marker_set(bool on) {
    ui_set_pace_marker(on);
    if (on == pace_marker) return;
    pace_marker = on;
    save("pace_mark", on ? 1 : 0);
    Serial.printf("Pace marker: %s\n", on ? "on" : "off");
}

bool pace_marker_get(void) { return pace_marker; }

void button_action_set(ButtonSlot slot, int32_t action) {
    if (slot < 0 || slot >= BTN_SLOT_COUNT) return;
    action = button_action_clamp(action, BTN_DEFAULTS[slot]);
    if (action == btn_actions[slot]) return;
    btn_actions[slot] = action;
    Preferences prefs;
    prefs.begin("clawdmeter", false);
    prefs.putInt(BTN_KEYS[slot], action);
    prefs.end();
    Serial.printf("Button %d action: 0x%lx\n", (int)slot + 1, (unsigned long)action);
}

int32_t button_action_get(ButtonSlot slot) {
    return (slot >= 0 && slot < BTN_SLOT_COUNT) ? btn_actions[slot] : BTN_ACT_NONE;
}

void theme_set(const uint32_t p[TH_COUNT]) {
    if (board_caps().mono_panel) return;   // 1-bit: the default palette only
    if (!theme_palette_set(p)) return;
    char hex[THEME_HEX_LEN + 1];
    theme_format_hex(p, hex);
    Preferences prefs;
    prefs.begin("clawdmeter", false);
    prefs.putBytes("theme", hex, THEME_HEX_LEN);
    prefs.end();
    ui_apply_theme();
    face_apply_theme();
    clawd_anim_recolor();
    mascot_repaint();
    performer_repaint();
    Serial.printf("Theme: %s\n", hex);
}
