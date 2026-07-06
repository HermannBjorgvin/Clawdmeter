#include "../../hal/sound_hal.h"
#include "board.h"

#if BOARD_HAS_SOUND

#include <Arduino.h>
#include "../../chime.h"

// AMOLED-2.16: ES8311 codec + speaker. The power amp is a plain GPIO. All the
// codec/I2S/playback work lives in the shared chime engine (../../chime.cpp);
// this file only supplies the board's pins and the amp-enable hook.

static void amp_enable(bool on) {
    digitalWrite(SND_PA_PIN, on ? HIGH : LOW);
}

void sound_hal_init(void) {
    pinMode(SND_PA_PIN, OUTPUT);
    const ChimeConfig cfg = {
        SND_I2S_MCLK, SND_I2S_BCLK, SND_I2S_WS, SND_I2S_DOUT, SND_I2S_DIN,
        SND_SAMPLE_RATE, SND_ES8311_ADDR, 65, amp_enable
    };
    chime_init(cfg);
}

void sound_hal_play_reset(void) { chime_play(); }
void sound_hal_tick(void)       { chime_tick(); }

#endif  // BOARD_HAS_SOUND
