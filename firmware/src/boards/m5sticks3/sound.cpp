#include "../../hal/sound_hal.h"
#include "board.h"

#if BOARD_HAS_SOUND

#include <Arduino.h>
#include "../../chime.h"
#include "pm1.h"

// ES8311 codec + AW8737 amp, shared chime engine. The amp enable is PM1
// GPIO3 over I2C rather than an ESP32 GPIO — driven per-play through the
// chime engine's hook so the speaker doesn't hiss between chimes.

static void amp_enable(bool on) {
    if (on) pm1_bit_on (PM1_REG_GPIO_OUT, 1 << 3);
    else    pm1_bit_off(PM1_REG_GPIO_OUT, 1 << 3);
}

void sound_hal_init(void) {
    // PM1 GPIO3 → gpio function, output, push-pull, low (amp off) — mirrors
    // M5Unified's "PA Control Pin Init" for this board.
    pm1_bit_off(PM1_REG_GPIO_FUNC0, 1 << 3);
    pm1_bit_on (PM1_REG_GPIO_MODE,  1 << 3);
    pm1_bit_off(PM1_REG_GPIO_DRV,   1 << 3);
    pm1_bit_off(PM1_REG_GPIO_OUT,   1 << 3);

    const ChimeConfig cfg = {
        SND_I2S_MCLK, SND_I2S_BCLK, SND_I2S_WS, SND_I2S_DOUT, SND_I2S_DIN,
        SND_SAMPLE_RATE, SND_ES8311_ADDR, 65, amp_enable
    };
    chime_init(cfg);
}

void sound_hal_play_reset(void) { chime_play(); }
void sound_hal_tick(void)       { chime_tick(); }

#endif  // BOARD_HAS_SOUND
