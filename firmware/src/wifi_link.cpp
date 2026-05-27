// Compile only when the WiFi transport is selected — keeps the USB CDC
// env from needing wifi_config.h (which only the user has) and from
// pulling in the WiFi / HTTPClient deps.
#ifdef CLAWDMETER_TRANSPORT_WIFI

#include "wifi_link.h"

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <string.h>

#include "wifi_config.h"

// Poll the bridge this often. Daemon assembles new payloads on its own
// 5s tick, so 5s on this side keeps display latency low without
// hammering the LAN.
#define WIFI_POLL_MS    5000
#define LINK_STALE_MS   15000   // longer than serial — WiFi has more jitter
#define HTTP_TIMEOUT_MS 4000

static char     data_buf[4096];
static bool     has_data = false;
static bool     ever_received = false;
static uint32_t last_data_ms = 0;
static uint32_t last_poll_ms = 0;
static char     port_label[64];

static void connect_wifi(void) {
    Serial.printf("WiFi: connecting to '%s'...\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void wifi_link_init(void) {
    has_data = false;
    ever_received = false;
    last_data_ms = 0;
    last_poll_ms = 0;
    snprintf(port_label, sizeof(port_label), "WiFi %s", BRIDGE_HOST);
    connect_wifi();
    Serial.println("READY");
}

static void poll_bridge(void) {
    if (WiFi.status() != WL_CONNECTED) return;

    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/payload", BRIDGE_HOST, BRIDGE_PORT);

    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(url)) {
        Serial.println("WiFi: http.begin failed");
        return;
    }
    http.addHeader("Authorization", "Bearer " BRIDGE_TOKEN);

    int code = http.GET();
    if (code == 200) {
        String body = http.getString();
        size_t len = body.length();
        if (len >= sizeof(data_buf)) len = sizeof(data_buf) - 1;
        memcpy(data_buf, body.c_str(), len);
        data_buf[len] = '\0';
        has_data = true;
        ever_received = true;
        last_data_ms = millis();
    } else if (code == 503) {
        // Daemon up but no payload assembled yet — quiet.
    } else if (code == 401) {
        Serial.println("WiFi: 401 — token mismatch with daemon");
    } else if (code > 0) {
        Serial.printf("WiFi: HTTP %d\n", code);
    } else {
        // Negative = connection-level error.
        Serial.printf("WiFi: poll error %d (%s)\n", code, http.errorToString(code).c_str());
    }
    http.end();
}

void wifi_link_tick(void) {
    static bool wifi_was_connected = false;
    bool wifi_now = (WiFi.status() == WL_CONNECTED);
    if (wifi_now && !wifi_was_connected) {
        Serial.printf("WiFi: connected, IP=%s\n", WiFi.localIP().toString().c_str());
        // Force first poll immediately on link-up.
        last_poll_ms = 0;
    } else if (!wifi_now && wifi_was_connected) {
        Serial.println("WiFi: disconnected, will reconnect...");
    }
    wifi_was_connected = wifi_now;

    if (!wifi_now) return;

    uint32_t now = millis();
    if (now - last_poll_ms >= WIFI_POLL_MS || last_poll_ms == 0) {
        last_poll_ms = now;
        poll_bridge();
    }
}

link_state_t wifi_link_get_state(void) {
    if (!ever_received) return LINK_STATE_WAITING;
    if ((millis() - last_data_ms) > LINK_STALE_MS) return LINK_STATE_STALE;
    return LINK_STATE_CONNECTED;
}

const char* wifi_link_get_port_name(void) {
    return port_label;
}

bool wifi_link_has_data(void) {
    return has_data;
}

const char* wifi_link_get_data(void) {
    has_data = false;
    return data_buf;
}

void wifi_link_send_ack(void) {
    Serial.println("ACK");  // log-only; HTTP has no out-of-band channel
}

void wifi_link_send_nack(void) {
    Serial.println("NACK");
}

void wifi_link_request_refresh(void) {
    // No-op. HTTP transport is poll-driven; the bridge doesn't have a
    // notify channel back to the firmware. The serial_link transport
    // uses this to ask the daemon for a fresh payload mid-poll-cycle;
    // here we just rely on the WIFI_POLL_MS cadence.
}

#endif // CLAWDMETER_TRANSPORT_WIFI
