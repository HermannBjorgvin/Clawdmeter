#include "board.h"
#include "io_expander.h"
#include <Arduino.h>
#include <Wire.h>
#include <esp_system.h>

// Shared I2C (GT911 + expander) then expander rails/backlight. The expander
// MUST come up before display_hal_begin() or the ST7701 stays unpowered.
extern "C" void board_init(void) {
    static const char* const reasons[] = {"unknown", "power-on", "ext", "software",
        "panic", "int-wdt", "task-wdt", "wdt", "deep-sleep", "BROWNOUT", "sdio",
        "usb", "jtag", "efuse", "pwr-glitch", "cpu-lockup"};
    int r = (int)esp_reset_reason();
    Serial.printf("Reset reason: %s (%d)\n",
                  r < (int)(sizeof(reasons) / sizeof(reasons[0])) ? reasons[r] : "?", r);
    // The expander survives ESP32 resets and may be stuck mid-transaction.
    io_expander_recover_bus();
    Wire.begin(IIC_SDA, IIC_SCL);
    delay(20);
    // One-line bus inventory — board revisions differ in which chips exist.
    Serial.print("I2C:");
    for (uint8_t a = 0x08; a < 0x78; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) Serial.printf(" 0x%02X", a);
    }
    Serial.println();
    io_expander_init();
}
