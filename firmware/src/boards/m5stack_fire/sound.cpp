#include "../../hal/sound_hal.h"

// M5Stack FIRE has a speaker, but it's driven from the ESP32 DAC on GPIO25 —
// not the I2S + ES8311 codec the shared chime engine (../../chime.cpp) targets.
// Rather than add a DAC playback path, sound output is a no-op for now (same
// posture as the AMOLED-2.06 / C6 ports). A future revision could wire the
// reset chime to a DAC tone here.

void sound_hal_init(void) {}
void sound_hal_tick(void) {}
void sound_hal_play_reset(void) {}
