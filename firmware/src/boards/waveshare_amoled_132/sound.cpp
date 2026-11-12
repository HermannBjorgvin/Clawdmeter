#include "../../hal/sound_hal.h"

// ES8311 is populated but its amp path is unverified on this board; no-op.
void sound_hal_init(void) {}
void sound_hal_tick(void) {}
void sound_hal_chime(void) {}
void sound_hal_tune(float, int) {}
