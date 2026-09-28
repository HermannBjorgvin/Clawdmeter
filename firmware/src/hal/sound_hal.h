#pragma once

// Optional audio output (a passive piezo buzzer driven by LEDC PWM). Used to
// chime when the Claude session limit resets. Boards without a buzzer — the
// AMOLED-1.8 and the C6, whose stock hardware has no speaker output — no-op on
// init/tick and ignore play requests.
//
// Playback is non-blocking: sound_hal_play_reset() only *queues* the chime and
// returns immediately; sound_hal_tick() (called every loop) advances the notes
// so the LVGL render loop never stalls.

void sound_hal_init(void);
void sound_hal_tick(void);
void sound_hal_play_reset(void);
// Short agent alerts: 1 beep = an agent finished, 2 = one is waiting on you.
// Boards that don't override it get a weak default (main.cpp) that plays the
// reset chime for 2+ and stays silent for 1.
void sound_hal_play_beeps(int count);
