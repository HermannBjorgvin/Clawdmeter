#include "../../hal/sound_hal.h"
#include "board.h"

#if BOARD_HAS_SOUND

#include <Arduino.h>
#include "../../chime.h"

// C6 AMOLED-2.16: ES8311 codec + speaker, same chime engine as the S3 1.8 and
// original 2.16 (../../chime.cpp). Unlike those boards there's no PA-enable
// line to drive — the official Waveshare XiaoZhi reference firmware for this
// exact board leaves its PA pin unset (GPIO_NUM_NC), so amp_enable is null
// here; chime.cpp already no-ops the hook when absent.
//
// I2C bus (SDA=8, SCL=7) is already up from board_init() by the time
// sound_hal_init() runs, shared with touch/PMU/IMU.

void sound_hal_init(void) {
    // 100 (max) clips audibly — the ES8311's volume register is linear
    // digital gain with no separate analog/PA stage on this board to fall
    // back on, so past a point it's just clipping the embedded PCM harder,
    // not getting cleanly louder. 85 trades a little loudness back for no
    // audible distortion.
    const ChimeConfig cfg = {
        SND_I2S_MCLK, SND_I2S_BCLK, SND_I2S_WS, SND_I2S_DOUT, SND_I2S_DIN,
        SND_SAMPLE_RATE, SND_ES8311_ADDR, 85, nullptr
    };
    chime_init(cfg);
}

void sound_hal_play_reset(void) { chime_play(); }
void sound_hal_tick(void)       { chime_tick(); }

#endif  // BOARD_HAS_SOUND
