#pragma once
// Firmware updates over Bluetooth (docs/protocol.md, "Firmware updates").
//
// ble.cpp hands over the owner's frames from the OTA characteristic and its
// link events; the receive buffer fills on the NimBLE host task, and the loop
// task does everything that touches flash in ota_tick(), so a sector erase
// never holds up Bluetooth. Replies go out on TX (ble_ota_reply).
//
// Nothing is written until someone allows the update on the board itself: a
// valid BEGIN shows "Update firmware?" for OTA_CONFIRM_SECS, and only a tap on
// the screen or a double button press starts it (OtaGate in ota_proto.h: the
// question belongs to one connection, ignores its first second, and an
// unanswered one blocks new ones for a while). Pairing alone (which trusts
// an identity address, with no PIN) is not enough to install code.
//
// A new image boots once in "pending" state. It is confirmed once the owner's
// computer is connected over an encrypted link and either a payload has been
// parsed or a minute has passed since; until then any reset (a crash, the
// power button) makes the bootloader go back to the previous firmware.
// Boards without a second app slot (the 4 MB ones) report a slot size of 0
// and refuse an update.
#include <stddef.h>
#include <stdint.h>

void ota_init(void);           // setup(): the slot sizes, and whether this image awaits confirmation
uint32_t ota_slot_size(void);  // bytes an update may take; 0 = USB only
bool ota_active(void);         // an update is under way (its screen is up)
void ota_tick(void);           // loop()
void ota_on_payload(void);     // loop(): a payload from the app parsed fine
// loop(): a button press while the question is up. Two in quick succession
// allow the update; once the question has settled every press goes to it and
// does nothing else (true: used up). A press in its first second is left alone.
bool ota_button_press(void);
// The serial `ota_allow` command: allows the waiting update at once.
bool ota_allow(void);

// From the NimBLE host task.
void ota_on_frame(const uint8_t* data, size_t len, uint16_t conn, uint16_t mtu);
void ota_on_disconnect(uint16_t conn);
void ota_on_owner_link(void);  // the owner's computer is connected and encrypted
