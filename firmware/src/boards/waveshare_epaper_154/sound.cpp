#include "../../hal/sound_hal.h"

// ePaper-1.54: no speaker, amp, or audio codec on this board — sound output
// is a no-op. The real ES8311 chime engine lives in ../../chime.cpp and is
// wired up by boards that have a verified amp path (see
// boards/waveshare_amoled_216/sound.cpp) behind BOARD_HAS_SOUND.

void sound_hal_init(void) {}
void sound_hal_tick(void) {}
void sound_hal_play_reset(void) {}
