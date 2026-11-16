#include "../../hal/sound_hal.h"
#include "../../buzzer.h"
#include "board.h"
#include "io_expander.h"
#include <Arduino.h>

// The LCD-2.1's buzzer is an active one switched by expander EXIO8: on/off
// only, no pitch or volume, so it can't play the ding. sound_hal_chime()
// plays three short beeps instead (alerts and session resets), and it beeps
// once at boot where the speaker boards ding, timed by the buzzer module
// without blocking; a failed "off" is retried until it lands (buzzer.h).
// io_expander_init() writes the latch with the buzzer off, so a reset
// mid-beep comes back silent. sound_hal_tune() has nothing to tune.

static bool set_buzzer(bool on) { return io_expander_set(IOX_BIT_BUZZER, on); }

void sound_hal_init(void)  { buzzer_init(set_buzzer, millis()); }
void sound_hal_tick(void)  { buzzer_tick(millis()); }
void sound_hal_chime(void) { buzzer_start(millis(), BUZZER_BEEPS); }

void sound_hal_tune(float gain, int volume) { (void)gain; (void)volume; }
