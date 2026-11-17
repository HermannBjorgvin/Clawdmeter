#pragma once

// Optional audio output: the ding (ding.h), played for an attention alert
// and when the session window resets. ES8311 boards stream it through the
// shared chime engine (chime.h). Boards with a plain on/off buzzer play beeps
// instead (buzzer.h: three per chime, one at boot). Boards without sound
// no-op on init/tick and ignore play requests.
//
// Playback is non-blocking: sound_hal_chime() only *queues* the sound and
// returns immediately, so the LVGL render loop never stalls.

void sound_hal_init(void);
void sound_hal_tick(void);
void sound_hal_chime(void);
// Speaker tuning (`chime <gain> [volume]`): try a gain (0..1) and codec volume (0..100,
// < 0 keeps it) until reboot, and play the ding. Buzzer boards just beep (nothing
// to tune); no-op without sound.
void sound_hal_tune(float gain, int volume);
