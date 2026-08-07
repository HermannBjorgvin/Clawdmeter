#include "../../hal/sound_hal.h"

// LCD-1.47: no codec, no amplifier, no buzzer — the kit has no audio path at
// all. The session-reset chime is therefore a no-op here (the shared engine
// lives in ../../chime.cpp and is wired up by the boards that do have a
// speaker, behind BOARD_HAS_SOUND).

void sound_hal_init(void) {}
void sound_hal_tick(void) {}
void sound_hal_play_reset(void) {}
