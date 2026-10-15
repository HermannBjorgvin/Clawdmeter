#include "../../hal/sound_hal.h"
#include "board.h"

#if BOARD_HAS_SOUND

#include <Arduino.h>
#include "../../chime.h"

// C6 AMOLED-2.16: ES8311 codec + NS4150B amp + speaker pads. Codec/I2S/playback
// live in the shared chime engine (../../chime.cpp, which renders a smaller
// mono ding into internal RAM on boards without PSRAM); this file only supplies
// the board's pins. No amp-enable hook: Waveshare's references for this board
// leave the PA pin unconnected. Wire is already up (board_init() ran it).

// Codec volume 65 is the level the PR author used on this board; not yet tuned
// by ear against the C#5 ding. Adjust live with `chime <gain> [volume]`.
#define CHIME_VOLUME 65

void sound_hal_init(void) {
    const ChimeConfig cfg = {
        SND_I2S_MCLK, SND_I2S_BCLK, SND_I2S_WS, SND_I2S_DOUT, SND_I2S_DIN,
        SND_SAMPLE_RATE, SND_ES8311_ADDR, CHIME_VOLUME, nullptr
    };
    chime_init(cfg);
}

void sound_hal_chime(void) { chime_play(); }
void sound_hal_tune(float gain, int volume) { chime_tune(gain, volume); }
void sound_hal_tick(void)  { chime_tick(); }

#endif  // BOARD_HAS_SOUND
