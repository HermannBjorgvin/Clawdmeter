#pragma once
#include <stdint.h>

// One-time codec + I2S init. Safe to call once after Wire.begin().
void audio_init(void);

// Play a short sine-wave beep. Blocks until the samples are queued (a few ms).
void audio_beep(uint32_t freq_hz, uint32_t duration_ms);

// Convenience: the "attention" notification sound.
void audio_attn_chime(void);

// Volume 0-100; 0 = silent (codec). Persists in RAM only.
void audio_set_volume(int pct);
int  audio_get_volume(void);

// Mute toggles the power amp (GPIO PA) — fully kills audio output.
void audio_set_muted(bool muted);
bool audio_is_muted(void);
