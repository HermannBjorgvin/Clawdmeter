#pragma once
#include <Arduino.h>

// WiFi station connectivity, independent of and additional to the BLE link.
// Credentials are provisioned over BLE (daemon writes {"src":"wifi",...} on
// the existing RX characteristic, see main.cpp's process_payload()) and
// persisted in NVS (Preferences, namespace "wifi") — there is no captive
// portal / AP-mode fallback in this phase.

enum wifi_state_t {
    WIFI_STATE_OFF,         // no credentials stored yet
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_FAILED,      // last connect attempt timed out; will retry
};

void wifi_init(void);          // call once from setup(); loads creds from NVS
void wifi_tick(void);          // call every loop(); non-blocking connect/retry

// Stores new credentials to NVS and (re)connects only if they actually
// changed. Called from main.cpp on a {"src":"wifi"} payload.
void wifi_set_credentials(const char* ssid, const char* pass);

wifi_state_t wifi_get_state(void);
String       wifi_get_ip(void);     // "" unless WIFI_STATE_CONNECTED

// Auth token for HTTP config-mutating endpoints (Phase 2+). Generated once
// (esp_random-backed) and persisted in the same "wifi" NVS namespace; this
// getter creates it on first call if absent.
const char* wifi_get_token(void);
