#include "audio.h"
#include "display_cfg.h"
#include <Arduino.h>
#include <ESP_I2S.h>
#include <math.h>

extern "C" {
#include "es8311.h"
}

#define SAMPLE_RATE     16000
#define VOICE_VOLUME    85
#define I2C_NUM         0
// Pins for the Waveshare 1.8" board's ES8311 codec.
#define PIN_I2S_MCLK    16
#define PIN_I2S_BCLK    9
#define PIN_I2S_WS      45
#define PIN_I2S_DOUT    8
#define PIN_I2S_DIN     10
#define PIN_PA_EN       46

static I2SClass        i2s;
static bool            audio_ok      = false;
static es8311_handle_t codec         = nullptr;
static int             cur_volume    = VOICE_VOLUME;
static bool            cur_muted     = false;

static bool codec_init(void) {
    codec = es8311_create(I2C_NUM, ES8311_ADDRRES_0);
    if (!codec) return false;
    const es8311_clock_config_t clk = {
        .mclk_inverted        = false,
        .sclk_inverted        = false,
        .mclk_from_mclk_pin   = true,
        .mclk_frequency       = SAMPLE_RATE * 256,
        .sample_frequency     = SAMPLE_RATE,
    };
    if (es8311_init(codec, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) return false;
    if (es8311_sample_frequency_config(codec, clk.mclk_frequency, clk.sample_frequency) != ESP_OK) return false;
    if (es8311_microphone_config(codec, false) != ESP_OK) return false;
    if (es8311_voice_volume_set(codec, cur_volume, NULL) != ESP_OK) return false;
    return true;
}

void audio_init(void) {
    pinMode(PIN_PA_EN, OUTPUT);
    digitalWrite(PIN_PA_EN, HIGH);

    i2s.setPins(PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT, PIN_I2S_DIN, PIN_I2S_MCLK);
    if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                   I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
        Serial.println("audio: I2S init failed");
        return;
    }
    if (!codec_init()) {
        Serial.println("audio: ES8311 init failed");
        return;
    }
    audio_ok = true;
    Serial.println("audio: ES8311 + I2S ready");
}

void audio_beep(uint32_t freq_hz, uint32_t duration_ms) {
    if (!audio_ok) return;

    const uint32_t n_samples = (SAMPLE_RATE * duration_ms) / 1000;
    // Stereo 16-bit: 4 bytes per sample-pair. Chunked to keep stack usage sane.
    const uint32_t CHUNK = 256;
    int16_t buf[CHUNK * 2];
    const float omega = 2.0f * 3.14159265f * (float)freq_hz / (float)SAMPLE_RATE;
    const uint32_t fade = SAMPLE_RATE / 50;  // 20ms attack/release

    uint32_t remaining = n_samples;
    uint32_t pos = 0;
    while (remaining > 0) {
        uint32_t batch = remaining < CHUNK ? remaining : CHUNK;
        for (uint32_t i = 0; i < batch; i++, pos++) {
            float env = 1.0f;
            if (pos < fade) env = (float)pos / (float)fade;
            else if (pos > n_samples - fade) env = (float)(n_samples - pos) / (float)fade;
            int16_t s = (int16_t)(sinf(omega * (float)pos) * env * 20000.0f);
            buf[i * 2 + 0] = s;
            buf[i * 2 + 1] = s;
        }
        i2s.write((const uint8_t*)buf, batch * 2 * sizeof(int16_t));
        remaining -= batch;
    }
}

void audio_attn_chime(void) {
    // Two-tone "ding" — rising minor third, reads as a notification.
    audio_beep(880, 120);
    audio_beep(1320, 180);
}

void audio_set_volume(int pct) {
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    cur_volume = pct;
    if (codec) es8311_voice_volume_set(codec, pct, NULL);
}

int audio_get_volume(void) { return cur_volume; }

void audio_set_muted(bool muted) {
    cur_muted = muted;
    // Toggling the PA pin physically cuts amplifier power — silent & saves
    // a hair of battery. Codec stays initialized so unmute is instant.
    digitalWrite(PIN_PA_EN, muted ? LOW : HIGH);
}

bool audio_is_muted(void) { return cur_muted; }
