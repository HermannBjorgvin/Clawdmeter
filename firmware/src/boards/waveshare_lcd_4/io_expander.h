#pragma once

#include <stdint.h>

// TCA9554 / CH32V003-as-expander on the LCD-4. Must run before
// display_hal_init() so the panel power rails and backlight are up.

// Frees a stuck I2C bus (9 clocks + STOP). Call before Wire.begin().
void io_expander_recover_bus(void);
void io_expander_init(void);
void io_expander_set_backlight(bool on);
// Buzzer enable (active buzzer: HIGH = tone). Returns false if the write
// failed — callers must retry an "off", or the tone latches on.
bool io_expander_set_buzzer(bool on);
// Backlight brightness 0..255 (CH32 PWM register; on/off only on TCA boards).
void io_expander_set_brightness(uint8_t level);
// Battery voltage in mV from the CH32's ADC, averaged; -1 if unavailable
// (TCA board revision, or the read failed).
int io_expander_battery_mv(void);
// Charger STAT line: 1 = charging, 0 = not charging, -1 = unavailable.
int io_expander_charging(void);
// True on the CH32V003 revision: the only one whose battery voltage is readable.
bool io_expander_has_battery(void);
