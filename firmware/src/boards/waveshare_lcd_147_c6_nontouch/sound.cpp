#include "../../hal/sound_hal.h"

// This board has no speaker, so the chime is a
// no-op.
void sound_hal_init(void) {}
void sound_hal_tick(void) {}
void sound_hal_chime(void) {}
void sound_hal_tune(float gain, int volume) { (void)gain; (void)volume; }
