#include "../../hal/sound_hal.h"

// The D1L's buzzer hangs off the RP2040 coprocessor, which this firmware does
// not talk to, and no codec is wired to the ESP32-S3. No audio path, so the
// chime is a no-op.
void sound_hal_init(void) {}
void sound_hal_tick(void) {}
void sound_hal_chime(void) {}
void sound_hal_tune(float gain, int volume) { (void)gain; (void)volume; }
