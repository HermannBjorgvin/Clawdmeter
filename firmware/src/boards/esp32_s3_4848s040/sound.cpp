#include "../../hal/sound_hal.h"

// No audio output is wired up in this port — the 4848S040 has no ES8311 codec
// and no buzzer pin broken out to the firmware. Every sound call is a no-op.

void sound_hal_init(void) {}
void sound_hal_tick(void) {}
void sound_hal_chime(void) {}
void sound_hal_tune(float, int) {}
