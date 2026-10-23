#include "../../hal/sound_hal.h"

// No audio hardware: the shared chime engine needs an ES8311 codec.
void sound_hal_init(void) {}
void sound_hal_tick(void) {}
void sound_hal_chime(void) {}
void sound_hal_tune(float gain, int volume) { (void)gain; (void)volume; }
