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
bool ble_has_bonds(void);
bool ble_has_data(void);
const char* ble_get_data(void);
void ble_send_ack(void);
void ble_send_nack(void);
void ble_request_refresh(void);

void ble_set_battery_level(int pct);

// Permission-approval channel: the host writes a pending tool-call request,
// the device shows it and notifies back the user's tapped decision.
bool ble_has_perm_request(void);
const char* ble_get_perm_request(void);  // clears the pending flag
void ble_send_perm_decision(const char* id, const char* decision);

// BLE HID keyboard
void ble_keyboard_press(uint8_t key, uint8_t modifier);
void ble_keyboard_release(void);
