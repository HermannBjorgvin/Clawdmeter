#include "chime.h"
#include <Arduino.h>
#include "ESP_I2S.h"
#include "es8311.h"
#include "bell_pcm.h"   // const uint8_t bell_pcm[] / bell_pcm_len — 44.1 kHz 16-bit stereo

// Shared ES8311 chime engine. See chime.h. Adapted from the original 2.16
// sound.cpp so the 2.16, 1.8 (and any future ES8311 board) share one copy of
// the codec setup, the embedded PCM, and the non-blocking playback task.

static I2SClass      i2s;
static ChimeConfig   cfg;
static bool          ready   = false;
static volatile bool playing = false;

static bool es8311_setup(void) {
    es8311_handle_t es = es8311_create(0, cfg.es8311_addr);   // I2C port 0 (shared Wire bus)
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

// One playback task serves both sources: the embedded PCM bell (alert_notes
// == NULL) and the synthesized alert melodies. The amp envelope — settle
// delay before, drain delay after — lives in exactly one place.
struct ToneNote { uint16_t freq; uint16_t ms; };   // freq 0 = rest
static const ToneNote* alert_notes = nullptr;
static int             alert_count = 0;

static void synth_tones(void);

static void playback_task(void* arg) {
    if (cfg.amp_enable) cfg.amp_enable(true);
    delay(8);                                  // let the amp settle (avoids turn-on pop)
    if (alert_notes) synth_tones();
    else             i2s.write((uint8_t*)bell_pcm, bell_pcm_len);
    delay(20);
    if (cfg.amp_enable) cfg.amp_enable(false);
    playing = false;
    vTaskDelete(nullptr);
}

static void start_playback(void) {
    playing = true;
    if (xTaskCreatePinnedToCore(playback_task, "chime", 4096, nullptr, 1, nullptr, 0) != pdPASS)
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
    Serial.println("chime: ES8311 ready");
    return true;
}

void chime_play(void) {
    if (!ready || playing) return;
    alert_notes = nullptr;   // → the embedded PCM bell
    start_playback();
}

// ---- Synthesized alert melodies -------------------------------------------
// Sine tones generated on the fly and streamed through the same I2S path.
// Each note gets a 5 ms linear fade in/out so note boundaries don't click.

static const ToneNote MELODY_INPUT[] = { {659, 220}, {0, 120}, {659, 220} };            // E5 E5 — «ждёт ответа»
static const ToneNote MELODY_PERM[]  = { {880, 110}, {0, 60}, {880, 110}, {0, 60}, {880, 160} };  // A5×3 — «нужно разрешение»
static const ToneNote MELODY_DONE[]  = { {523, 140}, {659, 140}, {784, 220} };          // C5 E5 G5 — «готово»
static const ToneNote MELODY_CAL[]   = { {784, 170}, {0, 40}, {1047, 170}, {0, 40}, {1319, 340} };  // G5 C6 E6 вверх, мягкое арпеджио — «скоро встреча»
static const ToneNote MELODY_CAL_GO[] = { {1047, 150}, {0, 50}, {1047, 150}, {0, 50}, {784, 320} }; // C6 C6 G5 «динь-динь-дон» — «встреча началась»
static const ToneNote MELODY_LIMIT[] = { {988, 130}, {880, 130}, {784, 240} };          // B5 A5 G5 вниз — «лимит близко»

#define NOTES(m) { m, (int)(sizeof(m) / sizeof(ToneNote)) }
static const struct { const ToneNote* notes; int count; } MELODIES[] = {
    NOTES(MELODY_INPUT),   // ATTN_INPUT
    NOTES(MELODY_PERM),    // ATTN_PERM
    NOTES(MELODY_DONE),    // ATTN_DONE
    NOTES(MELODY_CAL),     // ATTN_CAL
    NOTES(MELODY_CAL_GO),  // ATTN_CAL_START
    NOTES(MELODY_LIMIT),   // ATTN_LIMIT
};
#undef NOTES

static void synth_tones(void) {
    static int16_t buf[512];                        // 256 stereo frames per chunk
    const float amp = 6500.0f;
    for (int n = 0; n < alert_count; n++) {
        const ToneNote& note = alert_notes[n];
        const int total = cfg.sample_rate * note.ms / 1000;
        const int fade  = cfg.sample_rate * 5 / 1000;   // 5 ms envelope
        float phase = 0.0f;
        const float step = 2.0f * PI * note.freq / cfg.sample_rate;
        int done_frames = 0;
        while (done_frames < total) {
            int frames = min(256, total - done_frames);
            for (int i = 0; i < frames; i++) {
                int16_t s = 0;
                if (note.freq) {
                    float env = 1.0f;
                    int pos = done_frames + i;
                    if (pos < fade)              env = (float)pos / fade;
                    else if (total - pos < fade) env = (float)(total - pos) / fade;
                    s = (int16_t)(amp * env * sinf(phase));
                    phase += step;
                }
                buf[i * 2] = s; buf[i * 2 + 1] = s;
            }
            i2s.write((uint8_t*)buf, frames * 4);
            done_frames += frames;
        }
    }
}

void chime_play_alert(uint8_t kind) {
    if (!ready || playing) return;
    if (kind < 1 || kind > sizeof(MELODIES) / sizeof(MELODIES[0])) return;
    alert_notes = MELODIES[kind - 1].notes;
    alert_count = MELODIES[kind - 1].count;
    start_playback();
}

void chime_tick(void) {}   // playback runs in playback_task; nothing to poll
