#include "../../hal/sound_hal.h"
#include "board.h"
#include "pmu.h"
#include "../../chime.h"
#include "../../ding.h"

// NS4168 I2S amplifier on the shared chime engine, with no codec
// (es8311_addr 0): the engine scales the samples by SND_VOLUME.
//
// The amp stays powered from boot, as M5Unified keeps it while its speaker
// runs: its enable is a power-chip rail (ALDO3 on the v1.1, AXP192 GPIO2 on
// the v1.0), not a quick GPIO. Between dings the I2S driver's auto-clear
// sends silence. The small speaker turns the standard ding's strike and click
// into a crack, so this board plays the soft envelope (DING_SOFT, ding.h).

void sound_hal_init(void) {
    pmu_set_speaker(true);
    const ChimeConfig cfg = {
        -1 /* no MCLK */, SND_I2S_BCLK, SND_I2S_WS, SND_I2S_DOUT, -1 /* no mic */,
        SND_SAMPLE_RATE, 0 /* no codec */, SND_VOLUME, nullptr /* amp always on */,
        DING_SOFT
    };
    chime_init(cfg);
}

void sound_hal_tick(void)                   { chime_tick(); }
void sound_hal_chime(void)                  { chime_play(); }
void sound_hal_tune(float gain, int volume) { chime_tune(gain, volume); }
