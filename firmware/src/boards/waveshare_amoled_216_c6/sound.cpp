#include "../../hal/sound_hal.h"
#include "board.h"

#if BOARD_HAS_SOUND

#include <Arduino.h>
#include "../../chime.h"

// C6 AMOLED-2.16: ES8311 codec + NS4150B amp + speaker pads. Codec/I2S/playback
// live in the shared chime engine (../../chime.cpp); this file only supplies
// the board's pins. No amp-enable hook — Waveshare's references for this board
// leave the PA pin unconnected. Wire is already up (board_init() ran it).

void sound_hal_init(void) {
    const ChimeConfig cfg = {
        SND_I2S_MCLK, SND_I2S_BCLK, SND_I2S_WS, SND_I2S_DOUT, SND_I2S_DIN,
        SND_SAMPLE_RATE, SND_ES8311_ADDR, 65, nullptr
    };
    chime_init(cfg);
}

void sound_hal_play_reset(void) { chime_play(); }
void sound_hal_tick(void)       { chime_tick(); }

#endif  // BOARD_HAS_SOUND
