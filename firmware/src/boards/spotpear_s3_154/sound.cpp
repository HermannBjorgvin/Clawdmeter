#include "../../hal/sound_hal.h"
#include "board.h"

// No buzzer or audio codec on the SpotPear 1.54" board. main.cpp calls these
// unconditionally, so provide no-op definitions to satisfy the linker. (The
// AMOLED-1.8 port guards its real implementation behind #if BOARD_HAS_SOUND;
// here BOARD_HAS_SOUND is 0, so there's nothing to compile in — just stubs.)

void sound_hal_init(void)       {}
void sound_hal_tick(void)       {}
void sound_hal_play_reset(void) {}
