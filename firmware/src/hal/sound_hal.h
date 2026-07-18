#pragma once

// Optional audio output (a passive piezo buzzer driven by LEDC PWM). Used to
// chime when the Claude session limit resets. Boards without a buzzer — the
// AMOLED-1.8 and the C6, whose stock hardware has no speaker output — no-op on
// init/tick and ignore play requests.
//
// Playback is non-blocking: sound_hal_play_reset() only *queues* the chime and
// returns immediately; sound_hal_tick() (called every loop) advances the notes
// so the LVGL render loop never stalls.

#include <stdint.h>

void sound_hal_init(void);
void sound_hal_tick(void);
void sound_hal_play_reset(void);
// Synthesized alert melody; `kind` is an ATTN_* value from data.h
// (ATTN_INPUT / ATTN_PERM / ATTN_DONE / ATTN_CAL / ATTN_LIMIT). No-op on
// boards without a speaker.
void sound_hal_play_alert(uint8_t kind);
