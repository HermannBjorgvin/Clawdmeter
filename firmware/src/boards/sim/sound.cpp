#include "../../hal/sound_hal.h"
#include <stdio.h>

void sound_hal_init(void) {}
void sound_hal_tick(void) {}
void sound_hal_chime(void) { printf("[sim] chime! (session reset)\n"); }
void sound_hal_tune(float gain, int volume) { printf("[sim] chime! (gain %.2f, volume %d)\n", gain, volume); }
