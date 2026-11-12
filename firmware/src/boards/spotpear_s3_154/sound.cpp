#include "../../hal/sound_hal.h"

// No buzzer or audio codec is wired up for Clawdmeter on the SpotPear 1.54"
// MUMA; every sound call is a no-op.

void sound_hal_init(void)                   {}
void sound_hal_tick(void)                   {}
void sound_hal_chime(void)                  {}
void sound_hal_tune(float gain, int volume) { (void)gain; (void)volume; }
