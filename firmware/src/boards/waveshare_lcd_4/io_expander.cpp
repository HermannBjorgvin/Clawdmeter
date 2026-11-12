#include "io_expander.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// The expander is a separate chip that does NOT reset when the ESP32 resets
// (flash, RTS reset, crash). A reset mid-I2C-transaction can leave the bus
// stuck (a slave holding SDA low) and our init writes silently fail — while
// the expander keeps whatever state it had, buzzer included. That's the
// "continuous tone after flashing, fixed by a power cycle" failure. So, as
// Waveshare's own WS_CH32_IO library does: clock the bus free before
// Wire.begin() and retry the init writes. Writes from loop() paths (buzzer,
// backlight, battery reads) make one attempt; their callers retry on a timer,
// so a chip that stops answering never stalls the loop.
//
// CH32V003 (0x24): every pin is driven explicitly, nothing is left as a
// floating input, and the buzzer enable is LOW except while beeping. The
// backlight PWM register is written only once a battery has been seen (see
// io_expander_enable_pwm); until then the CH32 keeps its power-on PWM.
//
// TCA9554 (0x20): untested since this port first shipped, so it gets exactly
// the register writes it always has (init_tca) and nothing new: no buzzer,
// no PWM, no battery.

#define INIT_ATTEMPTS     5
#define RETRY_MS          200    // gap before a failed loop()-path write retries
#define REPROBE_MS        1000   // while no expander answered at boot

static uint8_t  expander_addr = 0;
static bool     is_ch32       = false;
static bool     found_late    = false;   // CH32 adopted by the re-probe
static uint8_t  output_latch  = 0;
static uint32_t reprobe_ms    = 0;

// CH32 backlight PWM: the level last asked for, the level the chip holds
// (-1 = not written this boot) and when a failed write may retry.
static bool     pwm_enabled   = false;
static int      pwm_want      = 255;
static int      pwm_written   = -1;
static bool     pwm_retry_pending = false;
static uint32_t pwm_retry_ms  = 0;

bool io_expander_recover_bus(void) {
    pinMode(IIC_SDA, INPUT_PULLUP);
    pinMode(IIC_SCL, OUTPUT_OPEN_DRAIN);
    digitalWrite(IIC_SCL, HIGH);
    delayMicroseconds(10);
    // Up to 9 clocks lets a slave finish the byte it's stuck in.
    for (int i = 0; i < 9 && digitalRead(IIC_SDA) == LOW; ++i) {
        digitalWrite(IIC_SCL, LOW);
        delayMicroseconds(10);
        digitalWrite(IIC_SCL, HIGH);
        delayMicroseconds(10);
    }
    // STOP condition: SDA low → high while SCL is high.
    pinMode(IIC_SDA, OUTPUT_OPEN_DRAIN);
    digitalWrite(IIC_SDA, LOW);
    delayMicroseconds(10);
    digitalWrite(IIC_SCL, HIGH);
    delayMicroseconds(10);
    digitalWrite(IIC_SDA, HIGH);
    delayMicroseconds(10);
    pinMode(IIC_SDA, INPUT_PULLUP);
    pinMode(IIC_SCL, INPUT_PULLUP);
    delay(2);
    return digitalRead(IIC_SDA) == HIGH && digitalRead(IIC_SCL) == HIGH;
}

static bool iox_probe(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

static bool iox_write(uint8_t reg, uint8_t val, int attempts = 1) {
    if (!expander_addr) return false;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (attempt) delay(10 * attempt);
        Wire.beginTransmission(expander_addr);
        Wire.write(reg);
        Wire.write(val);
        if (Wire.endTransmission() == 0) return true;
    }
    Serial.printf("LCD-4 IO expander: write reg 0x%02X failed\n", reg);
    return false;
}

// One register read, with a full STOP between the address write and the
// read — the CH32 firmware doesn't answer repeated-start reads reliably.
static bool iox_read_bytes(uint8_t reg, uint8_t* out, uint8_t len) {
    Wire.beginTransmission(expander_addr);
    Wire.write(reg);
    if (Wire.endTransmission(true) != 0) return false;
    if (Wire.requestFrom(expander_addr, len) != len) return false;
    for (uint8_t i = 0; i < len; i++) out[i] = Wire.read();
    return true;
}

static bool iox_commit_output(int attempts = 1) {
    if (is_ch32) return iox_write(CH32_REG_OUTPUT, output_latch, attempts);
    // TCA: the latch goes to 0x01 (output) and 0x02 (polarity, which only
    // affects input reads), as this port has always written it.
    bool ok = iox_write(TCA_REG_OUTPUT, output_latch, attempts);
    return iox_write(TCA_REG_POLARITY, output_latch, attempts) && ok;
}

#define CH32_OUT_DISPLAY_ON ((1u << CH32_PIN_SYS_EN) | (1u << CH32_PIN_LCD_RST) | \
                             (1u << CH32_PIN_TOUCH_RST))

static bool init_ch32(void) {
    // Waveshare's sequence is: all outputs, everything low, settle, then
    // power + release resets. But SYS_EN is also the battery power-hold latch:
    // after the PWR key starts battery power, SYS_EN high is what keeps it on.
    // Dropping it — on any ESP32 reset, e.g. the dip when USB is plugged in —
    // releases the hold, and the board then dies the moment USB is unplugged.
    // So SYS_EN stays high throughout; only the resets are pulsed (buzzer off).
    // Every pin an output, as Waveshare's library does. Making EXIO0 (charger
    // STAT) an input to read charging status made the buzzer sound continuously
    // on this board — so STAT is not read (see io_expander_charging).
    const uint8_t dir = 0xFF;
    output_latch = 1u << CH32_PIN_SYS_EN;
    bool ok = iox_write(CH32_REG_DIRECTION, dir, INIT_ATTEMPTS) &&
              iox_commit_output(INIT_ATTEMPTS);
    delay(200);
    // Release the resets whatever happened above: a dark panel is worse.
    output_latch = CH32_OUT_DISPLAY_ON;
    bool dir_ok = iox_write(CH32_REG_DIRECTION, dir, INIT_ATTEMPTS);
    bool out_ok = iox_commit_output(INIT_ATTEMPTS);
    delay(200);
    return ok && dir_ok && out_ok;
}

static bool init_tca(void) {
    // The port's original sequence, unchanged: output latch 0xFF, then config
    // 0x3A (bits 0/2/6/7 outputs, 1/3/4/5 inputs). See board.h for how that
    // lines up with the wiki's pin table.
    output_latch = TCA_OUTPUT_ON;
    bool ok = iox_commit_output(INIT_ATTEMPTS);
    return iox_write(TCA_REG_CONFIG, TCA_CONFIG, INIT_ATTEMPTS) && ok;
}

void io_expander_init(void) {
    for (int attempt = 0; attempt < 3 && !expander_addr; ++attempt) {
        if (iox_probe(IO_EXPANDER_ADDR)) {
            expander_addr = IO_EXPANDER_ADDR;
            is_ch32 = true;
        } else if (iox_probe(IO_EXPANDER_ADDR_ALT)) {
            expander_addr = IO_EXPANDER_ADDR_ALT;
            is_ch32 = false;
        } else {
            delay(20);
        }
    }
    if (!expander_addr) {
        Serial.println("LCD-4 IO expander not found (0x24/0x20); re-probing 0x24 every second");
        reprobe_ms = millis() + REPROBE_MS;
        return;
    }
    bool ok = is_ch32 ? init_ch32() : init_tca();
    Serial.printf("LCD-4 IO expander %s @ 0x%02X: %s (out=0x%02X)\n",
                  is_ch32 ? "CH32V003" : "TCA9554", expander_addr,
                  ok ? "OK" : "WRITE FAILED", output_latch);
    if (is_ch32)
        Serial.println(BOARD_HAS_BATTERY
            ? "LCD-4 backlight: CH32 PWM assumed inverted (reg 0x05 = 255 - level), "
              "written only once a battery is detected"
            : "LCD-4 backlight: CH32 PWM left at power-on (BOARD_HAS_BATTERY 0)");
}

static void pwm_try(void) {
    if (iox_write(CH32_REG_PWM, (uint8_t)(255 - pwm_want))) {
        pwm_written = pwm_want;
        pwm_retry_pending = false;
    } else {
        pwm_retry_pending = true;
        pwm_retry_ms = millis() + RETRY_MS;
    }
}

static bool pwm_due(void) {
    if (!pwm_enabled || pwm_want == pwm_written) return false;
    return !pwm_retry_pending || (int32_t)(millis() - pwm_retry_ms) >= 0;
}

void io_expander_set_brightness(uint8_t level) {
    if (!expander_addr) return;
    if (is_ch32) {
        // The register is inverted: higher = DIMMER (seen on a V4.0 board —
        // a dimmed level came out brighter than full). Callers pass 0..255
        // with 255 = full brightness, so pwm_try flips it. Without a battery
        // nothing is written (io_expander_enable_pwm).
        pwm_want = level;
        if (pwm_due()) pwm_try();
        return;
    }
    // TCA revision: on/off only, on the latch bit this port has always used.
    if (level > 0) output_latch |= (1u << TCA_PIN_BACKLIGHT);
    else           output_latch &= ~(1u << TCA_PIN_BACKLIGHT);
    iox_commit_output();
}

void io_expander_enable_pwm(void) {
    if (!expander_addr || !is_ch32 || pwm_enabled) return;
    pwm_enabled = true;
    Serial.printf("LCD-4 backlight: battery detected, PWM on (level %d)\n", pwm_want);
    pwm_try();
}

bool io_expander_set_buzzer(bool on) {
    // Nothing to drive: no expander, or a TCA board (its buzzer pin is left
    // an input by init_tca, so the latch bit would do nothing anyway).
    if (!expander_addr || !is_ch32) return true;
    uint8_t bit = 1u << CH32_PIN_BUZZER;
    if (on) output_latch |= bit;
    else    output_latch &= ~bit;
    return iox_commit_output();
}

void io_expander_set_backlight(bool on) {
    io_expander_set_brightness(on ? 255 : 0);
}

void io_expander_tick(void) {
    if (!expander_addr) {
        // A CH32 still wedged from before an ESP32 reset can come back later,
        // possibly with the buzzer latched on. Adopt it then: buzzer off and
        // the panel lines as init leaves them (no reset pulse, which would
        // blank a panel that's already running). Beeps work from then on;
        // the battery doesn't (the UI was built without it).
        if ((int32_t)(millis() - reprobe_ms) < 0) return;
        reprobe_ms = millis() + REPROBE_MS;
        if (!iox_probe(IO_EXPANDER_ADDR)) return;
        expander_addr = IO_EXPANDER_ADDR;
        is_ch32 = true;
        output_latch = CH32_OUT_DISPLAY_ON;
        if (!iox_write(CH32_REG_DIRECTION, 0xFF) || !iox_commit_output()) {
            expander_addr = 0;   // keep probing
            return;
        }
        found_late = true;
        Serial.println("LCD-4 IO expander CH32V003 answered late: buzzer off");
        return;
    }
    if (pwm_due()) pwm_try();
}

int io_expander_charging(void) {
    // Unavailable: reading STAT needs EXIO0 as an input, which set the buzzer
    // off (see init_ch32). Callers fall back to the voltage heuristic.
    return -1;
}

bool io_expander_has_battery(void) {
    return BOARD_HAS_BATTERY && expander_addr && is_ch32 && !found_late;
}

int io_expander_battery_mv(void) {
    if (!io_expander_has_battery()) return -1;
    uint32_t total = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t b[2];
        // One failed read ends the sample: the caller polls again later.
        if (!iox_read_bytes(CH32_REG_ADC, b, 2)) return -1;
        uint16_t raw = ((uint16_t)b[1] << 8) | b[0];
        total += raw > 1023 ? 1023 : raw;
    }
    float v = (total / 8.0f) * CH32_ADC_REF_V / 1023.0f * CH32_BAT_DIVIDER;
    return (int)(v * 1000.0f + 0.5f);
}
