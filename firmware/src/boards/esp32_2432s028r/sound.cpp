#include "../../hal/sound_hal.h"

// The CYD does have an audio path — GPIO 26 through a transistor amp to the
// on-board speaker — but the shared chime engine (chime.cpp) streams I2S into
// an ES8311 codec, which this board doesn't have. Driving the speaker would
// mean a second, PWM-based chime backend; until that exists, no-op (same
// posture as the LCD-4 and C6 ports).
void sound_hal_init(void) {}
void sound_hal_tick(void) {}
void sound_hal_play_reset(void) {}
