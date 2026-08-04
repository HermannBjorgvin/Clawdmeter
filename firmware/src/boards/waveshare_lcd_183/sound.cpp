#include "../../hal/sound_hal.h"

// LCD-1.83: no sound path wired up in this port, so output is a no-op. See
// boards/waveshare_lcd_154/sound.cpp for an ES8311 speaker reference if the
// onboard audio ever gets hooked up.

void sound_hal_init(void) {}
void sound_hal_tick(void) {}
void sound_hal_play_reset(void) {}
