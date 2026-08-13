#include "../../hal/sound_hal.h"

// No speaker or codec wired — the session-reset chime is silent.
// To add a passive buzzer later, see boards/waveshare_amoled_216/sound.cpp.

void sound_hal_init(void) {}
void sound_hal_tick(void) {}
void sound_hal_play_reset(void) {}
