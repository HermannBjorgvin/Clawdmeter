#pragma once
#include <stdint.h>

enum link_state_t {
    LINK_STATE_INIT,
    LINK_STATE_WAITING,    // serial open, no data yet
    LINK_STATE_CONNECTED,  // data received recently
    LINK_STATE_STALE,      // last data > LINK_STALE_MS ago
};

void serial_link_init(void);
void serial_link_tick(void);

link_state_t serial_link_get_state(void);
const char* serial_link_get_port_name(void);

bool serial_link_has_data(void);
const char* serial_link_get_data(void);
void serial_link_send_ack(void);
void serial_link_send_nack(void);
void serial_link_request_refresh(void);
