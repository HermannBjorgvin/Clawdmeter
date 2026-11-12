#pragma once

#include <stdint.h>

// TCA9554 / CH32V003-as-expander on the LCD-4. Must run before
// display_hal_init() so the panel power rails and backlight are up.

// Frees a stuck I2C bus (9 clocks + STOP). Call before Wire.begin().
// Returns true when SDA and SCL both read high afterwards.
bool io_expander_recover_bus(void);
// Probes 0x24 (CH32V003) then 0x20 (TCA9554) and programs whichever answers.
void io_expander_init(void);
// From loop(): retries a failed backlight write, and re-probes 0x24 while
// no expander has answered (a wedged CH32 may hold the buzzer on).
void io_expander_tick(void);
void io_expander_set_backlight(bool on);
// Buzzer enable (active buzzer: HIGH = tone; CH32 boards only, a no-op that
// returns true elsewhere). Returns false if the write failed — callers must
// retry an "off", or the tone latches on.
bool io_expander_set_buzzer(bool on);
// Backlight brightness 0..255. CH32: the PWM register, written only after
// io_expander_enable_pwm(); until then the level is remembered and the chip
// keeps its power-on PWM. TCA: on/off only.
void io_expander_set_brightness(uint8_t level);
// Lets the CH32 PWM register be written (the battery was detected) and
// applies the last requested level. Stays on for the rest of the boot.
void io_expander_enable_pwm(void);
// Battery voltage in mV from the CH32's ADC, 8 reads averaged; -1 if
// unavailable (TCA board revision, or a read failed — one try, no retry).
int io_expander_battery_mv(void);
// Charger STAT line: 1 = charging, 0 = not charging, -1 = unavailable.
int io_expander_charging(void);
// True on the CH32V003 revision found at boot (the only one whose battery
// voltage is readable), in a build with BOARD_HAS_BATTERY 1.
bool io_expander_has_battery(void);
