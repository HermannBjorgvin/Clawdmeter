#include "../../hal/sound_hal.h"

// LCD-1.47 (C6): no codec, no amplifier, no buzzer. The kit has no audio
// path at all, so every sound HAL call is a no-op. The shared engine lives in
// ../../chime.cpp and is wired up by the boards that do have a speaker.

void sound_hal_init(void) {}
void sound_hal_tick(void) {}
void sound_hal_chime(void) {}
void sound_hal_tune(float, int) {}
