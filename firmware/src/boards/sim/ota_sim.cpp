// Firmware updates don't exist in the simulator (no flash, no BLE): ota.cpp
// is left out of the sim build and these stand in. The update screen itself
// (ota_ui.cpp) is real, shown with the `ota_ui <pct>` command.
#include "../../ota.h"

void ota_init(void) {}
uint32_t ota_slot_size(void) { return 0; }
bool ota_active(void) { return false; }
void ota_tick(void) {}
void ota_on_payload(void) {}
bool ota_allow(void) { return false; }
bool ota_button_press(void) { return false; }
void ota_on_frame(const uint8_t*, size_t, uint16_t, uint16_t) {}
void ota_on_disconnect(uint16_t) {}
void ota_on_owner_link(void) {}
