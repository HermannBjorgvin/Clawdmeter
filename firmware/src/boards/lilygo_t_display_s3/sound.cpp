#include "../../hal/sound_hal.h"

// No audio output on the T-Display S3.

void sound_hal_init(void) {}
void sound_hal_tick(void) {}
void sound_hal_chime(void) {}
void sound_hal_tune(float gain, int volume) { (void)gain; (void)volume; }
