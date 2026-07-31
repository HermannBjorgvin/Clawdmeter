// BLE stub + scenario playback. Implements ble.h without any transport: a
// JSONL scenario file stands in for the daemon, delivered through the same
// ble_has_data()/ble_get_data() path main.cpp uses on hardware — so JSON
// parsing, usage-rate tracking, and the chime trigger all run for real.
#include "../../ble.h"
#include "sim_platform.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_STATES 64
#define MAX_LINE   512

struct SimState {
    char json[MAX_LINE];
    char name[32];
    uint32_t hold_ms;
};

static SimState states[MAX_STATES];
static int      n_states = 0;
static int      cur = 0;
static bool     playing = true;
static bool     connected = true;
static bool     pending = false;      // a state is queued for main's next poll
static uint32_t delivered_ms = 0;

static const char* FALLBACK[] = {
    "{\"name\":\"fresh\",\"s\":3.0,\"sr\":295,\"w\":12.0,\"wr\":9000,\"st\":\"allowed\",\"ok\":true}",
    "{\"name\":\"mid\",\"s\":48.0,\"sr\":150,\"w\":35.0,\"wr\":7200,\"st\":\"allowed\",\"ok\":true}",
    "{\"name\":\"high\",\"s\":92.0,\"sr\":30,\"w\":71.0,\"wr\":4600,\"st\":\"allowed\",\"ok\":true}",
    "{\"name\":\"reset+chime\",\"hold_ms\":4000,\"s\":2.0,\"sr\":298,\"w\":72.0,\"wr\":4500,\"st\":\"allowed\",\"c\":true,\"ok\":true}",
};

static void add_state(const char* line) {
    if (n_states >= MAX_STATES) return;
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
    if (!len || line[0] == '#') return;   // blank lines / comments
    SimState* s = &states[n_states];
    if (len >= MAX_LINE) len = MAX_LINE - 1;
    memcpy(s->json, line, len);
    s->json[len] = 0;
    s->hold_ms = 3000;
    snprintf(s->name, sizeof(s->name), "state %d", n_states + 1);
    // "name" and "hold_ms" ride along in the payload; main's parse_json
    // ignores unknown keys so the line is delivered as-is.
    JsonDocument doc;
    if (deserializeJson(doc, s->json) == DeserializationError::Ok) {
        s->hold_ms = doc["hold_ms"] | 3000;
        const char* nm = doc["name"] | (const char*)NULL;
        if (nm) snprintf(s->name, sizeof(s->name), "%s", nm);
    }
    n_states++;
}

static void load_scenario(void) {
    const char* tries[] = { getenv("SIM_SCENARIO"), "sim/scenario.jsonl",
                            "firmware/sim/scenario.jsonl", "../sim/scenario.jsonl" };
    FILE* f = NULL;
    for (const char* t : tries) {
        if (!t) continue;
        f = fopen(t, "r");
        if (f) { printf("[sim] scenario: %s\n", t); break; }
    }
    if (f) {
        char line[MAX_LINE];
        while (fgets(line, sizeof(line), f)) add_state(line);
        fclose(f);
    }
    if (!n_states) {
        printf("[sim] no scenario file found — using built-in states\n");
        for (const char* l : FALLBACK) add_state(l);
    }
}

static void refresh_title(void) {
    char t[96];
    snprintf(t, sizeof(t), "Clawdmeter sim — %s[%d/%d] %s %s",
             connected ? "" : "(disconnected) ",
             cur + 1, n_states, states[cur].name,
             playing ? "\xE2\x96\xB6" : "\xE2\x8F\xB8");
    sim_display_set_title(t);
}

void ble_init(void) {
    load_scenario();
    pending = true;
    refresh_title();
}

void ble_tick(void) {
    if (!connected || pending || !playing || n_states == 0) return;
    if (millis() - delivered_ms >= states[cur].hold_ms) {
        cur = (cur + 1) % n_states;
        pending = true;
        refresh_title();
    }
}

ble_state_t ble_get_state(void) {
    return connected ? BLE_STATE_CONNECTED : BLE_STATE_DISCONNECTED;
}
const char* ble_get_device_name(void) { return "Clawdmeter (sim)"; }
const char* ble_get_mac_address(void) { return "00:51:4D:00:00:01"; }

void ble_clear_bonds(void) { printf("[sim] pair gesture completed — bonds cleared\n"); }
bool ble_has_bonds(void)   { return true; }

bool ble_has_data(void) { return connected && pending; }
const char* ble_get_data(void) {
    pending = false;
    delivered_ms = millis();
    return states[cur].json;
}
void ble_send_ack(void)  {}
void ble_send_nack(void) { printf("[sim] payload NACKed — check the scenario JSON\n"); }
void ble_request_refresh(void) {}
void ble_set_battery_level(int pct) { (void)pct; }

void ble_keyboard_press(uint8_t key, uint8_t modifier) {
    printf("[sim] HID press key=0x%02X mod=0x%02X\n", key, modifier);
}
void ble_keyboard_release(void) { printf("[sim] HID release\n"); }

// ---- Playback controls (called from the sim_platform event pump) ----
void sim_playback_toggle(void) {
    playing = !playing;
    delivered_ms = millis();   // restart the hold timer on resume
    refresh_title();
}
void sim_playback_step(int dir) {
    if (!n_states) return;
    playing = false;
    cur = (cur + dir + n_states) % n_states;
    pending = true;
    refresh_title();
}
void sim_playback_jump(int idx) {
    if (idx < 0 || idx >= n_states) return;
    playing = false;
    cur = idx;
    pending = true;
    refresh_title();
}
void sim_playback_toggle_link(void) {
    connected = !connected;
    refresh_title();
}
