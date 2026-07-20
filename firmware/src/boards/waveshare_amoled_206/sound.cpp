#include "../../hal/sound_hal.h"

// AMOLED-2.06: an ES8311 codec (plus an ES7210 ADC) sits on the shared I2C bus,
// but this port never brings that path up and it's unverified on hardware — so
// sound output is a no-op. The real ES8311 chime engine lives in ../../chime.cpp
// and is wired up by the 2.16 and S3 1.8 ports behind BOARD_HAS_SOUND; enable it
// here once the amp pin and codec wiring are confirmed on this board.

void sound_hal_init(void) {}
void sound_hal_tick(void) {}
void sound_hal_play_reset(void) {}
void sound_hal_play_alert(uint8_t kind) { (void)kind; }
