#include "../../hal/sound_hal.h"

// Stock hardware has an ES8311 I2S codec + speaker, not the simple LEDC
// piezo buzzer this HAL targets (see BOARD_HAS_SOUND in board.h). Wiring up
// the session-reset chime through the I2S codec is possible later — mirror
// boards/waveshare_amoled_18/sound.cpp's chime_init() pattern with this
// board's I2S pins — but is out of scope for the initial port.

void sound_hal_init(void) {}
void sound_hal_tick(void) {}
void sound_hal_play_reset(void) {}
