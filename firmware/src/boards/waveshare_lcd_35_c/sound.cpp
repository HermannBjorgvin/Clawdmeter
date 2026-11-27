#include "../../hal/sound_hal.h"
#include "board.h"

#if BOARD_HAS_SOUND

#include <Arduino.h>
#include "../../chime.h"
#include "io_expander.h"

// LCD-3.5-C: ES8311 codec + NS4150B speaker amp. Codec/I2S/playback live in
// the shared chime engine (../../chime.cpp); this file supplies the board's
// pins and the amp-enable hook. The amp's CTRL is TCA9554 EXIO7, already an
// output (board_init() ran io_expander_init()).

static void amp_enable(bool on) { io_expander_set(IOX_PA_CTRL, on); }

void sound_hal_init(void) {
    // power_hal_init() has just switched on ALDO1 (the codec's rail): give it
    // time to settle before the codec is probed.
    delay(100);
    const ChimeConfig cfg = {
        SND_I2S_MCLK, SND_I2S_BCLK, SND_I2S_WS, SND_I2S_DOUT, SND_I2S_DIN,
        SND_SAMPLE_RATE, SND_ES8311_ADDR, SND_VOLUME, amp_enable
    };
    chime_init(cfg);
}

void sound_hal_chime(void) { chime_play(); }
void sound_hal_tune(float gain, int volume) { chime_tune(gain, volume); }
void sound_hal_tick(void)       { chime_tick(); }

#endif  // BOARD_HAS_SOUND
