#include "wifi_net.h"
#include <WiFi.h>
#include <Preferences.h>
#include <string.h>

#define WIFI_NVS_NS         "wifi"
#define CONNECT_TIMEOUT_MS  15000UL
#define RETRY_BACKOFF_MS    30000UL
#define TOKEN_LEN           8

static wifi_state_t state        = WIFI_STATE_OFF;
static char         ssid_buf[33] = "";
static char         pass_buf[65] = "";
static char         token_buf[TOKEN_LEN + 1] = "";
static uint32_t     connect_start_ms = 0;
static uint32_t     next_retry_ms    = 0;
static bool         have_creds       = false;

static void load_credentials(void) {
    Preferences p;
    // Read-write, not read-only: opening a namespace that has never been
    // created before in read-only mode crashes on this esp32-arduino version
    // (assert failed: xQueueSemaphoreTake, right after the expected nvs_open
    // NOT_FOUND) -- read-write auto-creates the namespace instead of hitting
    // that path. "wifi" is empty on a fresh flash, so this always applies on
    // first boot after upgrading to this firmware.
    if (!p.begin(WIFI_NVS_NS, false)) return;
    String s  = p.getString("ssid", "");
    String pw = p.getString("pass", "");
    String t  = p.getString("token", "");
    p.end();
    if (s.length() > 0) {
        strlcpy(ssid_buf, s.c_str(), sizeof(ssid_buf));
        strlcpy(pass_buf, pw.c_str(), sizeof(pass_buf));
        have_creds = true;
    }
    if (t.length() == TOKEN_LEN) {
        strlcpy(token_buf, t.c_str(), sizeof(token_buf));
    }
}

static void save_credentials(const char* ssid, const char* pass) {
    Preferences p;
    if (!p.begin(WIFI_NVS_NS, false)) return;
    p.putString("ssid", ssid);
    p.putString("pass", pass);
    p.end();
}

static void start_connect(void) {
    if (!have_creds) return;
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid_buf, pass_buf);
    state = WIFI_STATE_CONNECTING;
    connect_start_ms = millis();
}

void wifi_init(void) {
    load_credentials();
    // Deliberately skip WiFi.mode() entirely when there are no credentials
    // yet: esp_wifi_init() (triggered by WiFi.mode()) needs a large one-shot
    // heap allocation (RX/TX buffer pools, ~50-70 KB) that this no-PSRAM
    // board can't always spare on top of the full LVGL UI + BLE. Not calling
    // it costs nothing -- web_server_tick() already defers WebServer::begin()
    // until WIFI_STATE_CONNECTED, which can now only happen once real
    // credentials trigger start_connect() below, so there's no lwIP/socket
    // path left that needs the network stack up prematurely.
    if (have_creds) start_connect();
}

void wifi_tick(void) {
    switch (state) {
    case WIFI_STATE_OFF:
        break;
    case WIFI_STATE_CONNECTING:
        if (WiFi.status() == WL_CONNECTED) {
            state = WIFI_STATE_CONNECTED;
        } else if (millis() - connect_start_ms > CONNECT_TIMEOUT_MS) {
            state = WIFI_STATE_FAILED;
            next_retry_ms = millis() + RETRY_BACKOFF_MS;
        }
        break;
    case WIFI_STATE_CONNECTED:
        if (WiFi.status() != WL_CONNECTED) {
            state = WIFI_STATE_FAILED;
            next_retry_ms = millis() + RETRY_BACKOFF_MS;
        }
        break;
    case WIFI_STATE_FAILED:
        if (millis() >= next_retry_ms) start_connect();
        break;
    }
}

void wifi_set_credentials(const char* ssid, const char* pass) {
    if (strcmp(ssid, ssid_buf) == 0 && strcmp(pass, pass_buf) == 0) return;  // unchanged, no-op
    strlcpy(ssid_buf, ssid, sizeof(ssid_buf));
    strlcpy(pass_buf, pass, sizeof(pass_buf));
    have_creds = (strlen(ssid_buf) > 0);
    save_credentials(ssid_buf, pass_buf);
    if (have_creds) {
        start_connect();
    } else {
        WiFi.disconnect(true);
        state = WIFI_STATE_OFF;
    }
}

wifi_state_t wifi_get_state(void) { return state; }

String wifi_get_ip(void) {
    if (state != WIFI_STATE_CONNECTED) return "";
    return WiFi.localIP().toString();
}

// Excludes visually-ambiguous characters (0/O, 1/I) since this gets read off
// a 135px-wide screen and typed into a browser.
static const char TOKEN_CHARSET[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

const char* wifi_get_token(void) {
    if (token_buf[0] != '\0') return token_buf;
    for (int i = 0; i < TOKEN_LEN; i++) {
        token_buf[i] = TOKEN_CHARSET[esp_random() % (sizeof(TOKEN_CHARSET) - 1)];
    }
    token_buf[TOKEN_LEN] = '\0';
    Preferences p;
    if (p.begin(WIFI_NVS_NS, false)) {
        p.putString("token", token_buf);
        p.end();
    }
    return token_buf;
}
