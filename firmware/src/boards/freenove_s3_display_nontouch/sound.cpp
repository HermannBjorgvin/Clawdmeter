#include "../../hal/sound_hal.h"

// The kit's speaker output is not wired up in this port, so the chime is a
// no-op.
void sound_hal_init(void) {}
void sound_hal_tick(void) {}
void sound_hal_chime(void) {}
void sound_hal_tune(float gain, int volume) { (void)gain; (void)volume; }
