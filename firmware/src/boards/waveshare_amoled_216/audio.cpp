#include "../../hal/audio_hal.h"

// No codec or speaker on the AMOLED-2.16. Stubs satisfy the HAL contract;
// callers gate on board_caps().has_audio so these never get hit in practice.
void audio_hal_init(void) {}
void audio_hal_play_chime(void) {}
void audio_hal_set_muted(bool /*muted*/) {}
bool audio_hal_is_muted(void) { return true; }
