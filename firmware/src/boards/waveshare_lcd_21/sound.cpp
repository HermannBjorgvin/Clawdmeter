#include "../../hal/sound_hal.h"

// The LCD-2.1's buzzer is an active one switched by expander EXIO8: on/off
// only, no pitch, so it can't play the ding. board_init() keeps it off and
// every sound call is a no-op.

void sound_hal_init(void) {}
void sound_hal_tick(void) {}
void sound_hal_chime(void) {}
void sound_hal_tune(float, int) {}
