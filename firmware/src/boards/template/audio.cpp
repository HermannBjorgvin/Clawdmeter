#include "../../hal/audio_hal.h"

// Stub audio HAL for boards without a codec. Replace with a real
// implementation if your board has audio output (see boards/waveshare_amoled_18/audio.cpp).
void audio_hal_init(void) {}
void audio_hal_play_chime(void) {}
void audio_hal_set_muted(bool /*muted*/) {}
bool audio_hal_is_muted(void) { return true; }
