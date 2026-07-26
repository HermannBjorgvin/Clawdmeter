#include "../../hal/sound_hal.h"

// C6 AMOLED-2.16 has no buzzer wired, so sound output is a no-op. See
// boards/waveshare_amoled_216/sound.cpp for the real LEDC buzzer driver.

void sound_hal_init(void) {}
void sound_hal_tick(void) {}
void sound_hal_play_reset(void) {}
