#include "chime.h"
#include <Arduino.h>
#include "ESP_I2S.h"
#include "es8311.h"
#include <string.h>
#include <esp_heap_caps.h>
#include <soc/soc_caps.h>
#include "ding.h"

// Shared ES8311 chime engine. See chime.h. Every ES8311 board shares one copy
// of the codec setup and the non-blocking playback task.
//
// The ding is rendered once into a mono buffer and streamed from there, so
// nothing is computed while the DMA waits: computing it live starved the DMA
// (ESP_I2S keeps ~33 ms of buffers and doesn't let us grow them) and
// crackled. The render runs in a background task started on the first
// chime_tick(), after setup() has made its own allocations, so boot never
// waits for it. A ding asked for during the render plays when it finishes;
// that is also how the boot ding plays.
//
// The buffer lives in PSRAM where there is some. Without PSRAM (the C6) it
// takes internal RAM only while CHIME_RAM_RESERVE stays free, and a ding that
// doesn't fit is cut short with a fade. Live synthesis is left as a last
// resort for chips with an FPU; without one (the C6) it can't keep up.

// Share of the ding's own level (it peaks just under full scale): headroom
// for the codec and the small amp. ChimeConfig.volume sets the loudness.
#define DING_GAIN 0.7f
#define CHUNK_FRAMES 512
// Above the Arduino loop (LVGL); on core 1 where there is one, away from the
// NimBLE host and radio on core 0.
#define CHIME_TASK_PRIORITY 10
#define CHIME_TASK_CORE (portNUM_PROCESSORS > 1 ? 1 : 0)
// Internal RAM left free when the buffer can't go to PSRAM.
#define CHIME_RAM_RESERVE (48 * 1024)
// A buffer shorter than this isn't worth it.
#define CHIME_MIN_S 0.5f
// Fade over the end of a ding cut short.
#define CHIME_FADE_S 0.03f

static I2SClass      i2s;
static ChimeConfig   cfg;
static bool          ready   = false;
static volatile bool playing = false;
static es8311_handle_t es    = nullptr;

static float    gain       = DING_GAIN;
static int16_t* pcm        = nullptr;   // mono samples of the rendered ding
static uint32_t pcm_frames = 0;         // how many fit (may be < ding_frames)
static float    pcm_gain   = -1.0f;     // gain `pcm` was rendered at
static volatile bool render_started = false;
static volatile bool rendering = false;  // the background render is running
static volatile bool pending   = false;  // play when the render finishes
static portMUX_TYPE  state_lock = portMUX_INITIALIZER_UNLOCKED;  // rendering + pending

static bool es8311_setup(void) {
    es = es8311_create(0, cfg.es8311_addr);   // I2C port 0 (shared Wire bus)
    if (!es) return false;
    // mclk_inverted, sclk_inverted, mclk_from_mclk_pin, mclk_frequency, sample_frequency
    const es8311_clock_config_t clk = {
        false, false, true, cfg.sample_rate * 256, cfg.sample_rate
    };
    if (es8311_init(es, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) return false;
    es8311_sample_frequency_config(es, clk.mclk_frequency, clk.sample_frequency);
    es8311_microphone_config(es, false);
    es8311_voice_volume_set(es, cfg.volume, NULL);
    return true;
}

static int16_t chunk[CHUNK_FRAMES * 2];      // stereo frames, off the task stack

// Find room for the buffer: PSRAM, else internal RAM above the reserve
// (possibly fewer frames than the whole ding). Sets pcm / pcm_frames.
static bool allocate(void) {
    const uint32_t rate = (uint32_t)cfg.sample_rate, want = ding_frames(rate);
    pcm = (int16_t*)heap_caps_malloc((size_t)want * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pcm) {
        pcm_frames = want;
        return true;
    }
    const size_t block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t room = block > CHIME_RAM_RESERVE ? (block - CHIME_RAM_RESERVE) / sizeof(int16_t) : 0;
    const uint32_t frames = room < want ? (uint32_t)room : want;
    Serial.printf("chime: largest internal block %u bytes, room for %u of %u frames\n",
                  (unsigned)block, (unsigned)frames, (unsigned)want);
    if (frames < (uint32_t)(CHIME_MIN_S * rate)) return false;
    pcm = (int16_t*)heap_caps_malloc((size_t)frames * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!pcm) return false;
    pcm_frames = frames;
    return true;
}

// Render the ding at the current gain into `pcm` (allocated on first use).
// False when there's no room.
static bool render(void) {
    if (!pcm && !allocate()) return false;
    const uint32_t rate = (uint32_t)cfg.sample_rate, full = ding_frames(rate), frames = pcm_frames;
    const uint32_t fade = frames < full ? (uint32_t)(CHIME_FADE_S * rate) : 0;
    const float g = gain;                       // chime_tune may change it meanwhile
    const uint32_t t0 = millis();
    for (uint32_t n = 0; n < frames; n++) {
        float s = (float)ding_sample(n, rate, g);
        if (n + fade >= frames) s *= (float)(frames - n) / (float)(fade + 1);
        pcm[n] = (int16_t)s;
        if ((n & 2047) == 2047) vTaskDelay(1);  // let the idle task (and its watchdog) run
    }
    pcm_gain = g;
    Serial.printf("chime: rendered %u frames at %d Hz, gain %.2f, in %lu ms (at %lu ms since boot)\n",
                  (unsigned)frames, cfg.sample_rate, g, (unsigned long)(millis() - t0),
                  (unsigned long)millis());
    return true;
}

static void start_playback(void);

static void render_task(void* arg) {
    if (!render()) Serial.println("chime: no room to pre-render the ding");
    portENTER_CRITICAL(&state_lock);
    rendering = false;
    const bool play = pending;
    pending = false;
    portEXIT_CRITICAL(&state_lock);
    if (play) start_playback();
    vTaskDelete(nullptr);
}

// Stream `pcm` (mono) as stereo frames.
static void stream_buffer(void) {
    for (uint32_t n = 0; n < pcm_frames;) {
        uint32_t k = 0;
        for (; k < CHUNK_FRAMES && n < pcm_frames; k++, n++) {
            chunk[2 * k] = pcm[n];
            chunk[2 * k + 1] = pcm[n];
        }
        i2s.write((uint8_t*)chunk, k * 2 * sizeof(int16_t));
    }
}

// Compute and stream the ding as it plays (no buffer; FPU chips only).
static void stream_live(void) {
    const uint32_t rate = (uint32_t)cfg.sample_rate, frames = ding_frames(rate);
    for (uint32_t n = 0; n < frames;) {
        uint32_t k = 0;
        for (; k < CHUNK_FRAMES && n < frames; k++, n++) {
            const int16_t s = ding_sample(n, rate, gain);
            chunk[2 * k] = s;
            chunk[2 * k + 1] = s;
        }
        i2s.write((uint8_t*)chunk, k * 2 * sizeof(int16_t));
    }
}

static void chime_task(void* arg) {
    // A new gain (chime_tune) renders again here, before the amp comes on.
    const bool buffered = pcm && (pcm_gain == gain || render());
    if (cfg.amp_enable) cfg.amp_enable(true);
    delay(8);                                  // let the amp settle (avoids turn-on pop)
    if (buffered) stream_buffer();
#if SOC_CPU_HAS_FPU
    else stream_live();
#endif
    memset(chunk, 0, sizeof(chunk));           // a little silence so the DMA drains
    i2s.write((uint8_t*)chunk, sizeof(chunk));
    delay(20);
    if (cfg.amp_enable) cfg.amp_enable(false);
    playing = false;
    vTaskDelete(nullptr);
}

static void start_playback(void) {
    if (playing) return;
    playing = true;
    if (xTaskCreatePinnedToCore(chime_task, "chime", 4096, nullptr, CHIME_TASK_PRIORITY, nullptr,
                                CHIME_TASK_CORE) != pdPASS)
        playing = false;   // couldn't spawn — stay silent rather than wedge the flag
}

bool chime_init(const ChimeConfig& c) {
    cfg = c;
    if (cfg.amp_enable) cfg.amp_enable(false);   // amp off until we play

    i2s.setPins(cfg.bclk, cfg.ws, cfg.dout, cfg.din, cfg.mclk);
    if (!i2s.begin(I2S_MODE_STD, cfg.sample_rate, I2S_DATA_BIT_WIDTH_16BIT,
                   I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
        Serial.println("chime: I2S init failed");
        return false;
    }
    if (!es8311_setup()) {
        Serial.println("chime: ES8311 init failed");
        return false;
    }
    ready = true;
    pending = true;   // the boot ding, once the render is done
    Serial.println("chime: ES8311 ready");
    return true;
}

void chime_play(void) {
    if (!ready) return;
    portENTER_CRITICAL(&state_lock);
    const bool wait = !render_started || rendering;
    if (wait) pending = true;
    portEXIT_CRITICAL(&state_lock);
    if (!wait) start_playback();
}

void chime_tick(void) {
    // First tick: setup() is done, so render now, in the background, at the
    // lowest priority (the loop and the BLE host keep the CPU).
    if (!ready || render_started) return;
    portENTER_CRITICAL(&state_lock);
    render_started = true;
    rendering = true;
    portEXIT_CRITICAL(&state_lock);
    if (xTaskCreatePinnedToCore(render_task, "chime-render", 4096, nullptr, tskIDLE_PRIORITY, nullptr, 0) != pdPASS) {
        portENTER_CRITICAL(&state_lock);
        rendering = false;   // render on the first play instead
        const bool play = pending;
        pending = false;
        portEXIT_CRITICAL(&state_lock);
        if (play) start_playback();
    }
}

void chime_tune(float g, int volume) {
    if (!ready || playing) return;
    gain = g < 0.0f ? 0.0f : (g > 1.0f ? 1.0f : g);   // re-rendered on the next play
    if (volume >= 0 && es) es8311_voice_volume_set(es, volume > 100 ? 100 : volume, NULL);
    chime_play();
}
