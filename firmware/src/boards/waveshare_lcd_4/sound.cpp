#include "../../hal/sound_hal.h"
#include "../../buzzer_pulse.h"
#include "io_expander.h"
#include <Arduino.h>

// Active buzzer on the expander's BEE_EN pin: HIGH sounds a fixed tone, with
// no pitch or volume, so the shared ding (ding.h) is not used here.
// sound_hal_chime() plays three short beeps instead (alerts and session
// resets), and it beeps once at boot where the speaker boards ding, timed by
// buzzer_pulse without blocking. Each write is one I2C
// attempt; a failed "off" is retried until it lands (buzzer_pulse.h).
// sound_hal_tune() has nothing to tune.

void sound_hal_init(void)  { buzzer_pulse_init(io_expander_set_buzzer, millis()); }
void sound_hal_tick(void)  { buzzer_pulse_tick(millis()); }
void sound_hal_chime(void) { buzzer_pulse_start(millis(), BUZZER_BEEPS); }

void sound_hal_tune(float gain, int volume) { (void)gain; (void)volume; }
