#include "../../hal/sound_hal.h"

// C6 AMOLED-1.8: an ES8311 codec sits on the shared I2C bus and the audio amp
// is gated by TCA9554 P7, but this port never brings that path up (P7 is left
// off in io_expander_init) and it's unverified on hardware — so sound output
// is a no-op. The real ES8311 chime engine lives in ../../chime.cpp and is
// wired up by the S3 sibling (boards/waveshare_amoled_18/sound.cpp) behind
// BOARD_HAS_SOUND; enable it here once the amp path is verified on this board.

void sound_hal_init(void) {}
void sound_hal_tick(void) {}
void sound_hal_play_reset(void) {}
