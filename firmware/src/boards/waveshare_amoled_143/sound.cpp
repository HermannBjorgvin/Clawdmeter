#include "../../hal/sound_hal.h"

// AMOLED-1.43: the audio path is not wired up in this port, so every sound HAL
// call is a no-op. The shared chime engine (../../chime.cpp) is ready for a
// board that confirms its codec and amplifier pins on hardware.

void sound_hal_init(void) {}
void sound_hal_tick(void) {}
void sound_hal_chime(void) {}
void sound_hal_tune(float, int) {}
