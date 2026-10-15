#pragma once
#include <stdint.h>

enum ble_state_t {
    BLE_STATE_INIT,
    BLE_STATE_ADVERTISING,
    BLE_STATE_CONNECTED,
    BLE_STATE_DISCONNECTED,
};

void ble_init(void);
void ble_tick(void);
ble_state_t ble_get_state(void);
const char* ble_get_device_name(void);
const char* ble_get_mac_address(void);
void ble_clear_bonds(void);
void ble_print_status(void);   // serial `ble`: state, bonds, owner, peers
void ble_disconnect_all(void); // serial `disconnect`: drop every link
bool ble_has_data(void);
// The latest payload, copied for the caller; valid until the next call.
const char* ble_get_data(void);
void ble_send_ack(void);
void ble_send_nack(void);
void ble_send_dismiss(void);   // TX {"dismiss":true}: the user ended the alert on the device
void ble_request_refresh(void);

void ble_set_battery_level(int pct);

// BLE HID keyboard
void ble_keyboard_press(uint8_t key, uint8_t modifier);
void ble_keyboard_release(void);
