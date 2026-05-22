#pragma once
#include <stdbool.h>

// Audio HAL — boards with `has_audio` capability implement this; boards
// without it provide a no-op stub. Shared code calls these unconditionally
// after checking `board_caps().has_audio` so the call sites stay free of
// `#ifdef BOARD_*`.
//
// Lifecycle:
//   audio_hal_init()        — once during setup(). Brings up the codec
//                             (I2C config, I2S DMA) and spawns the
//                             playback task. Safe to call before BLE.
//   audio_hal_play_chime()  — fire-and-forget. Returns immediately;
//                             actual playback happens on the audio task.
//                             Coalesces back-to-back calls (a chime
//                             already in flight swallows the new one).
//   audio_hal_set_muted()   — toggles a soft mute. When muted,
//                             play_chime() is a no-op. Persisted to NVS
//                             by the caller, not by the HAL.

#ifdef __cplusplus
extern "C" {
#endif

void audio_hal_init(void);
void audio_hal_play_chime(void);
void audio_hal_set_muted(bool muted);
bool audio_hal_is_muted(void);

#ifdef __cplusplus
}
#endif
