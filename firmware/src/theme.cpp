#include "theme.h"
#include <Preferences.h>
#include <Arduino.h>

// Shares the "clawdmeter" namespace with brightness.cpp -- one namespace for
// the handful of user-chosen settings that should outlive a reboot.
#define PREF_NS   "clawdmeter"
#define PREF_KEY  "mode"
#define PREF_AUTO "autoflip"

static theme_mode_t active_mode = THEME_MODE_CLAUDE;
static uint8_t autoflip_idx = 0;

void theme_init(void) {
    Preferences prefs;
    prefs.begin(PREF_NS, true);
    uint8_t saved = prefs.getUChar(PREF_KEY, 0xFF);
    prefs.end();

    if (saved < THEME_MODE_COUNT) active_mode = (theme_mode_t)saved;

    prefs.begin(PREF_NS, true);
    autoflip_idx = prefs.getUChar(PREF_AUTO, 0);
    if (autoflip_idx >= AUTOFLIP_COUNT) autoflip_idx = 0;
    prefs.end();

    Serial.printf("Theme init: mode=%s %s\n",
                  THEMES[active_mode].name, AUTOFLIP_LABEL[autoflip_idx]);
}

const Theme& theme(void) {
    return THEMES[active_mode];
}

theme_mode_t theme_mode(void) {
    return active_mode;
}

void theme_set_mode(theme_mode_t mode) {
    if (mode < 0 || mode >= THEME_MODE_COUNT) return;
    if (mode == active_mode) return;         // no write when nothing changed
    active_mode = mode;

    Preferences prefs;
    prefs.begin(PREF_NS, false);
    prefs.putUChar(PREF_KEY, (uint8_t)active_mode);
    prefs.end();
}

uint8_t theme_autoflip_idx(void) {
    return autoflip_idx;
}

uint32_t theme_autoflip_ms(void) {
    return AUTOFLIP_MS[autoflip_idx];
}

const char* theme_autoflip_label(void) {
    return AUTOFLIP_LABEL[autoflip_idx];
}

void theme_cycle_autoflip(void) {
    autoflip_idx = (autoflip_idx + 1) % AUTOFLIP_COUNT;

    Preferences prefs;
    prefs.begin(PREF_NS, false);
    prefs.putUChar(PREF_AUTO, autoflip_idx);
    prefs.end();
}

theme_mode_t theme_next_mode(void) {
    return (theme_mode_t)((active_mode + 1) % THEME_MODE_COUNT);
}
