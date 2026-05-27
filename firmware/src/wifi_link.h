#pragma once
#include <stdint.h>

// WiFi/HTTP transport — alternative to serial_link.{cpp,h} for the
// no-USB-cable setup. ESP32 connects to the user's WiFi network and polls
// the daemon's local HTTP bridge over the LAN. Compile-time selectable via
// the CLAWDMETER_TRANSPORT_WIFI build flag.
//
// Public API mirrors serial_link.h so main.cpp can swap transports with
// just an #include + #define choice.

enum link_state_t {
    LINK_STATE_INIT,
    LINK_STATE_WAITING,    // wifi connecting / no payload yet
    LINK_STATE_CONNECTED,  // payload received recently
    LINK_STATE_STALE,      // last payload > LINK_STALE_MS ago
};

void wifi_link_init(void);
void wifi_link_tick(void);

link_state_t wifi_link_get_state(void);
const char* wifi_link_get_port_name(void);    // returns "WiFi <bridge-host>"

bool wifi_link_has_data(void);
const char* wifi_link_get_data(void);
void wifi_link_send_ack(void);     // serial log only — HTTP doesn't have an out-of-band ACK
void wifi_link_send_nack(void);    // serial log only
void wifi_link_request_refresh(void);  // no-op; HTTP is poll-driven, no notify channel
