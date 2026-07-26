#include "../../hal/sound_hal.h"
#include "board.h"

#if BOARD_HAS_SOUND

#include <Arduino.h>
#include "../../chime.h"

// AMOLED-1.8: ES8311 codec + speaker. Codec/I2S/playback live in the shared
// chime engine (../../chime.cpp); this file supplies the board's pins and the
// amp-enable hook. io_expander is already up (board_init() ran it).
//
// The amp enable is GPIO 46 ONLY. Do NOT drive XCA9554 EXIO2 low here: on the
// SH8601+FT3168 revision, pulling EXIO2 low takes the FT3168 touch controller
// off the I2C bus until EXIO2 is raised again (verified on hardware by
// bisection — an earlier "drive both amp candidates, the unused one is
// harmless" version of this hook silently killed touch for the whole session,
// since amp_enable(false) runs at chime_init to park the amp off).
// io_expander_init() parks EXIO2 HIGH and it must stay that way.

static void amp_enable(bool on) {
    digitalWrite(SND_PA_PIN, on ? HIGH : LOW);   // GPIO 46
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
