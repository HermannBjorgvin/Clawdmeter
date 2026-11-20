#pragma once
#include <stddef.h>
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
void ble_hold_advertising(void); // stop advertising until the restart (a finished update)
bool ble_has_data(void);
// The latest payload, copied for the caller; valid until the next call.
const char* ble_get_data(void);
void ble_send_ack(void);
void ble_send_nack(void);
void ble_send_dismiss(void);   // TX {"dismiss":true}: the user ended the alert on the device
// A firmware-update reply (ota_proto.h) on TX to `conn` (0xFFFF: every subscriber).
void ble_ota_reply(const uint8_t* data, size_t len, uint16_t conn);
void ble_request_refresh(void);

void ble_set_battery_level(int pct);

// BLE HID keyboard: a whole report: [modifiers, 0, key1..key6]
void ble_keyboard_report(const uint8_t report[8]);
// BLE HID consumer control (media keys): one usage down, then 0 = released
void ble_consumer_press(uint16_t usage);
void ble_consumer_release(void);
