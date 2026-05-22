// Audio HAL for Waveshare ESP32-S3-Touch-AMOLED-1.8.
//
// Hardware: ES8311 mono codec (I2C-configured) fed by the ESP32-S3 I2S0
// peripheral; codec output drives an onboard PA whose enable line is on
// GPIO 46. Pin map + init values cross-checked against Waveshare's own
// `examples/15_ES8311/15_ES8311.ino` for this exact SKU.
//
// Playback flow:
//   audio_hal_init()        — configures the codec + I2S DMA + spawns a
//                             playback task that waits on a semaphore.
//   audio_hal_play_chime()  — gives the semaphore; the task synthesizes
//                             two short G6 beeps (180 ms each, 70 ms
//                             gap) into a heap buffer, drives PA enable
//                             HIGH, writes to I2S, then drops PA enable
//                             LOW to silence the amp. Frequencies +
//                             timings live in NOTE_FREQS_HZ / *_MS so
//                             tweaking the chime is a one-line edit.
//
// The codec runs in slave mode; the ESP32 generates BCLK + LRCK + MCLK.
// Sample rate is fixed at 16 kHz, 16-bit mono — plenty for a chime, keeps
// DMA buffers tiny.

#include "../../hal/audio_hal.h"
#include "board.h"

#include <Arduino.h>
#include <Wire.h>
#include <ESP_I2S.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <math.h>

// ----- Chime parameters -----
// All hard-coded here rather than in a header because nothing else needs
// them; calibrate on hardware then commit the final values.
static constexpr uint32_t SAMPLE_RATE_HZ   = 16000;
static constexpr uint32_t NOTE_DURATION_MS = 180;    // tone-on per note
static constexpr uint32_t GAP_MS           = 70;     // silence between notes
static constexpr uint16_t NUM_NOTES        = 2;
// Two beeps at G6 — minimal, recognizably "ding-ding", not pretending to
// be a melody. Same frequency keeps the codec/speaker firmly in its
// comfortable range.
static constexpr float    NOTE_FREQS_HZ[NUM_NOTES] = { 1567.98f, 1567.98f };
static constexpr float    PEAK_AMPLITUDE   = 0.65f;  // fraction of int16 range
static constexpr uint32_t ATTACK_SAMPLES   = SAMPLE_RATE_HZ * 6 / 1000;   // 6 ms ramp in
static constexpr uint32_t RELEASE_SAMPLES  = SAMPLE_RATE_HZ * 22 / 1000;  // 22 ms ramp out

// ----- ES8311 register addresses (subset; full map in datasheet) -----
static constexpr uint8_t ES8311_REG00 = 0x00;  // reset
static constexpr uint8_t ES8311_REG01 = 0x01;  // clock manager
static constexpr uint8_t ES8311_REG02 = 0x02;  // clock manager (clk div)
static constexpr uint8_t ES8311_REG03 = 0x03;  // ADC FS / OSR
static constexpr uint8_t ES8311_REG04 = 0x04;  // DAC OSR
static constexpr uint8_t ES8311_REG05 = 0x05;  // ADC/DAC clock div
static constexpr uint8_t ES8311_REG06 = 0x06;  // BCLK / LRCK config
static constexpr uint8_t ES8311_REG07 = 0x07;
static constexpr uint8_t ES8311_REG08 = 0x08;
static constexpr uint8_t ES8311_REG09 = 0x09;  // SDP in (DAC input format)
static constexpr uint8_t ES8311_REG0A = 0x0A;  // SDP out (ADC output format)
static constexpr uint8_t ES8311_REG0B = 0x0B;
static constexpr uint8_t ES8311_REG0C = 0x0C;
static constexpr uint8_t ES8311_REG0D = 0x0D;  // system power
static constexpr uint8_t ES8311_REG0E = 0x0E;  // analog power
static constexpr uint8_t ES8311_REG10 = 0x10;  // analog bias
static constexpr uint8_t ES8311_REG11 = 0x11;
static constexpr uint8_t ES8311_REG12 = 0x12;
static constexpr uint8_t ES8311_REG13 = 0x13;
static constexpr uint8_t ES8311_REG14 = 0x14;  // ADC input selection
static constexpr uint8_t ES8311_REG16 = 0x16;
static constexpr uint8_t ES8311_REG17 = 0x17;  // ADC gain
static constexpr uint8_t ES8311_REG32 = 0x32;  // DAC volume
static constexpr uint8_t ES8311_REG37 = 0x37;  // DAC unmute / ramp
static constexpr uint8_t ES8311_REG44 = 0x44;  // ADC->DAC loopback / output mixer

static I2SClass i2s;
static SemaphoreHandle_t play_sem = nullptr;
static volatile bool muted_flag = false;
static bool init_ok = false;

static bool es8311_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES8311_I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

// Minimal ES8311 init for 16 kHz mono playback in I2S slave mode.
// Sequence derived from espressif/esp-adf's `es8311.c` and the Waveshare
// vendor demo. If anything sounds wrong (distorted, silent, too quiet),
// the DAC volume (REG32) and output mixer (REG44) are the first knobs.
static bool es8311_init_codec(void) {
    // Probe the codec before doing anything else — saves a confusing
    // pile of "Wire.endTransmission failed" logs if it's absent.
    Wire.beginTransmission(ES8311_I2C_ADDR);
    if (Wire.endTransmission() != 0) {
        Serial.println("ES8311 not found at 0x18; audio disabled");
        return false;
    }
    bool ok = true;
    // Reset and bring out of reset.
    ok &= es8311_write(ES8311_REG00, 0x1F); delay(20);
    ok &= es8311_write(ES8311_REG00, 0x00);
    // Clock manager — slave mode, MCLK from MCLK pin, no clk div.
    ok &= es8311_write(ES8311_REG01, 0x30);
    ok &= es8311_write(ES8311_REG02, 0x00);
    ok &= es8311_write(ES8311_REG03, 0x10);
    ok &= es8311_write(ES8311_REG16, 0x24);
    ok &= es8311_write(ES8311_REG04, 0x10);
    ok &= es8311_write(ES8311_REG05, 0x00);
    // System / bias.
    ok &= es8311_write(ES8311_REG0B, 0x00);
    ok &= es8311_write(ES8311_REG0C, 0x00);
    ok &= es8311_write(ES8311_REG10, 0x1F);
    ok &= es8311_write(ES8311_REG11, 0x7F);
    // Re-assert slave master setting + power up.
    ok &= es8311_write(ES8311_REG00, 0x80);
    ok &= es8311_write(ES8311_REG01, 0x3F);
    // I2S format: 16-bit, standard Philips I2S in both directions.
    ok &= es8311_write(ES8311_REG09, 0x0C);
    ok &= es8311_write(ES8311_REG0A, 0x0C);
    // System power on.
    ok &= es8311_write(ES8311_REG0D, 0x01);
    ok &= es8311_write(ES8311_REG0E, 0x02);
    ok &= es8311_write(ES8311_REG12, 0x00);
    ok &= es8311_write(ES8311_REG13, 0x10);
    ok &= es8311_write(ES8311_REG14, 0x1A);
    // DAC volume — 0xBF ≈ -32 dB, conservative starting point.
    // Bumping toward 0xFF is louder; calibrate on hardware before raising.
    ok &= es8311_write(ES8311_REG32, 0xBF);
    // Output mixer: route DAC to output; bit3 = DAC unmute, ramp enabled.
    ok &= es8311_write(ES8311_REG37, 0x08);
    return ok;
}

static void synthesize_and_play(void) {
    // Per-note slot = tone + silence. Building one contiguous buffer lets
    // us hand the whole chime to I2S in a single write — DMA handles the
    // rest. Silence between notes is what makes the three pitches
    // perceptible as separate steps rather than blurring into one tone.
    const uint32_t samples_per_tone = SAMPLE_RATE_HZ * NOTE_DURATION_MS / 1000;
    const uint32_t samples_per_gap  = SAMPLE_RATE_HZ * GAP_MS / 1000;
    const uint32_t samples_per_slot = samples_per_tone + samples_per_gap;
    const uint32_t total_samples = samples_per_slot * NUM_NOTES;
    int16_t* buf = (int16_t*)heap_caps_malloc(total_samples * sizeof(int16_t), MALLOC_CAP_DEFAULT);
    if (!buf) {
        Serial.println("chime: heap alloc failed");
        return;
    }
    memset(buf, 0, total_samples * sizeof(int16_t));
    const float two_pi_over_sr = 2.0f * (float)M_PI / (float)SAMPLE_RATE_HZ;
    for (uint16_t n = 0; n < NUM_NOTES; n++) {
        const float w = NOTE_FREQS_HZ[n] * two_pi_over_sr;
        const uint32_t base = n * samples_per_slot;
        for (uint32_t i = 0; i < samples_per_tone; i++) {
            // Linear attack / linear release envelope. Avoids the click
            // you'd get from a hard gate clipping the sine mid-cycle.
            float env = 1.0f;
            if (i < ATTACK_SAMPLES) env = (float)i / (float)ATTACK_SAMPLES;
            else if (i + RELEASE_SAMPLES > samples_per_tone)
                env = (float)(samples_per_tone - i) / (float)RELEASE_SAMPLES;
            const float s = sinf(w * (float)i) * env * PEAK_AMPLITUDE;
            buf[base + i] = (int16_t)(s * 32767.0f);
        }
        // Trailing GAP_MS of the slot stays zero (memset above).
    }
    digitalWrite(PA_CTRL_GPIO, HIGH);
    delay(2);  // amp wake-up; ~1 ms in Waveshare's demo
    i2s.write((uint8_t*)buf, total_samples * sizeof(int16_t));
    // Hold PA on until DMA fully drains. Small over-estimate to be safe.
    delay((NOTE_DURATION_MS + GAP_MS) * NUM_NOTES + 40);
    digitalWrite(PA_CTRL_GPIO, LOW);
    free(buf);
}

static void play_task(void* /*arg*/) {
    for (;;) {
        if (xSemaphoreTake(play_sem, portMAX_DELAY) == pdTRUE) {
            if (muted_flag) continue;
            synthesize_and_play();
        }
    }
}

void audio_hal_init(void) {
    pinMode(PA_CTRL_GPIO, OUTPUT);
    digitalWrite(PA_CTRL_GPIO, LOW);  // start silent until first chime

    if (!es8311_init_codec()) {
        Serial.println("ES8311 init failed; audio disabled");
        return;
    }

    // I2S0 in standard (Philips) mode, slave to nobody — we generate the
    // clocks. Mono 16-bit at 16 kHz.
    i2s.setPins(I2S_BCLK_GPIO, I2S_LRCK_GPIO, I2S_DOUT_GPIO, I2S_DIN_GPIO, I2S_MCLK_GPIO);
    if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE_HZ, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
        Serial.println("I2S begin failed; audio disabled");
        return;
    }

    play_sem = xSemaphoreCreateBinary();
    if (!play_sem) {
        Serial.println("audio: sem alloc failed");
        return;
    }
    // Stack size: synthesize_and_play allocates the PCM buffer on the heap,
    // so the task's own stack only needs room for FreeRTOS overhead + sinf.
    BaseType_t r = xTaskCreatePinnedToCore(play_task, "audio", 4096, nullptr, 1, nullptr, 1);
    if (r != pdPASS) {
        Serial.println("audio: task spawn failed");
        return;
    }
    init_ok = true;
    Serial.println("Audio HAL ready");
}

void audio_hal_play_chime(void) {
    if (!init_ok || muted_flag || !play_sem) return;
    // Coalesce: if a chime is already in flight, drop this one. Avoids a
    // pile-up if the daemon glitches and sends c=1 many times in a row.
    xSemaphoreGive(play_sem);
}

void audio_hal_set_muted(bool muted) { muted_flag = muted; }
bool audio_hal_is_muted(void) { return muted_flag; }
