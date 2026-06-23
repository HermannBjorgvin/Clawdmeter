#include "board.h"
#include <Arduino.h>
#include <esp_system.h>

// SPI is brought up inside display_hal_init().
// No IO expander, no I2C peripherals are used by this port.
extern "C" void board_init(void) {
    // ---- Latch the battery power rail ON, as early as possible ----
    // On battery, pressing the PWR side-button only momentarily applies
    // power. We must immediately drive VBAT_PWR HIGH to hold the rail on;
    // otherwise power collapses the instant the button is released and the
    // e-paper keeps the half-drawn splash frame (its bistable nature). This
    // runs before display_hal_init()/display_hal_begin() so the rail is
    // held before the SSD1681 ever receives a byte — mirroring the
    // Waveshare 07_BATT_PWR_Test user_app_init() ordering. Harmless on USB.
    pinMode(VBAT_PWR_GPIO, OUTPUT);
    digitalWrite(VBAT_PWR_GPIO, VBAT_PWR_ON_LEVEL);

    // ---- Boot diagnostics ----
    // Log the reset reason so a brownout loop (repeated ESP_RST_BROWNOUT)
    // is distinguishable from a clean boot when debugging on battery.
    Serial.printf("{\"reset_reason\":%d}\n", (int)esp_reset_reason());
}
