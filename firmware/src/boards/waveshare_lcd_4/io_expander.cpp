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
// Wire.begin(), retry every write, and read the result back.
//
// Every pin is driven explicitly: nothing is left as a floating input, and
// the buzzer enable is always LOW.

static uint8_t expander_addr = 0;
static bool    is_ch32       = false;
static uint8_t output_latch  = 0;

void io_expander_recover_bus(void) {
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
}

static bool iox_probe(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

static bool iox_write(uint8_t reg, uint8_t val) {
    if (!expander_addr) return false;
    for (int attempt = 0; attempt < 5; ++attempt) {
        Wire.beginTransmission(expander_addr);
        Wire.write(reg);
        Wire.write(val);
        if (Wire.endTransmission() == 0) return true;
        delay(10 + attempt * 10);
    }
    Serial.printf("LCD-4 IO expander: write reg 0x%02X failed\n", reg);
    return false;
}

// Register read with a full STOP between the address write and the read —
// the CH32 firmware doesn't answer repeated-start reads reliably.
static bool iox_read_bytes(uint8_t reg, uint8_t* out, uint8_t len) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        Wire.beginTransmission(expander_addr);
        Wire.write(reg);
        if (Wire.endTransmission(true) == 0 &&
            Wire.requestFrom(expander_addr, len) == len) {
            for (uint8_t i = 0; i < len; i++) out[i] = Wire.read();
            return true;
        }
        delay(5);
    }
    return false;
}

static int iox_read(uint8_t reg) {
    uint8_t v;
    return iox_read_bytes(reg, &v, 1) ? v : -1;
}

static bool iox_commit_output(void) {
    return iox_write(is_ch32 ? CH32_REG_OUTPUT : TCA_REG_OUTPUT, output_latch);
}

static bool init_ch32(void) {
    // Waveshare's sequence is: all outputs, everything low, settle, then
    // power + release resets. But SYS_EN is also the battery power-hold latch:
    // after the PWR key starts battery power, SYS_EN high is what keeps it on.
    // Dropping it — on any ESP32 reset, e.g. the dip when USB is plugged in —
    // releases the hold, and the board then dies the moment USB is unplugged.
    // So SYS_EN stays high throughout; only the resets are pulsed (buzzer off).
    output_latch = (1u << CH32_PIN_SYS_EN) | (1u << CH32_PIN_LCD_RST) |
                   (1u << CH32_PIN_TOUCH_RST);
    // Every pin an output, as Waveshare's library does. Making EXIO0 (charger
    // STAT) an input to read charging status made the buzzer sound continuously
    // on this board — so STAT is not read (see io_expander_charging).
    const uint8_t dir = 0xFF;
    bool ok = iox_write(CH32_REG_DIRECTION, dir) &&
              iox_write(CH32_REG_OUTPUT, 1u << CH32_PIN_SYS_EN);
    delay(200);
    ok = ok && iox_write(CH32_REG_DIRECTION, dir) && iox_commit_output();
    delay(200);
    return ok;
}

static bool init_tca(void) {
    // Latch first so pins come up in a known state, then make all but
    // RTC_INT outputs. Resets released, backlight on, SD deselected, buzzer off.
    output_latch = (1u << TCA_PIN_TP_RST) | (1u << TCA_PIN_BACKLIGHT) |
                   (1u << TCA_PIN_LCD_RST) | (1u << TCA_PIN_SD_CS) |
                   (1u << TCA_PIN_BLC);
    bool ok = iox_commit_output() &&
              iox_write(TCA_REG_CONFIG, 1u << TCA_PIN_RTC_INT);
    if (ok) {   // a real TCA9554 reads back exactly what we wrote
        int out = iox_read(TCA_REG_OUTPUT), cfg = iox_read(TCA_REG_CONFIG);
        if (out != output_latch || cfg != (1 << TCA_PIN_RTC_INT)) {
            Serial.printf("LCD-4 IO expander: TCA readback mismatch out=0x%02X cfg=0x%02X\n", out, cfg);
        }
    }
    return ok;
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
        Serial.println("LCD-4 IO expander not found (0x24/0x20)");
        return;
    }
    bool ok = is_ch32 ? init_ch32() : init_tca();
    Serial.printf("LCD-4 IO expander %s @ 0x%02X: %s (out=0x%02X, buzzer off)\n",
                  is_ch32 ? "CH32V003" : "TCA9554", expander_addr,
                  ok ? "OK" : "WRITE FAILED", output_latch);
}

void io_expander_set_brightness(uint8_t level) {
    if (!expander_addr) return;
    if (is_ch32) {
        // The register is inverted: higher = DIMMER (seen on a V4.0 board —
        // the "dimmed" on-battery level came out brighter than full on USB).
        // Callers pass 0..255 with 255 = full brightness, so flip it here.
        static int last = -1;
        if (level == last) return;   // fades call this every 20 ms
        if (iox_write(CH32_REG_PWM, 255 - level)) last = level;
        return;
    }
    // TCA revision: BL_EN is on/off only.
    uint8_t before = output_latch;
    if (level > 0) output_latch |= (1u << TCA_PIN_BACKLIGHT);
    else           output_latch &= ~(1u << TCA_PIN_BACKLIGHT);
    if (output_latch != before) iox_commit_output();
}

bool io_expander_set_buzzer(bool on) {
    if (!expander_addr) return false;
    uint8_t bit = 1u << (is_ch32 ? CH32_PIN_BUZZER : TCA_PIN_BUZZER);
    if (on) output_latch |= bit;
    else    output_latch &= ~bit;
    return iox_commit_output();
}

void io_expander_set_backlight(bool on) {
    io_expander_set_brightness(on ? 255 : 0);
}

int io_expander_charging(void) {
    // Unavailable: reading STAT needs EXIO0 as an input, which set the buzzer
    // off (see init_ch32). Callers fall back to the voltage heuristic.
    return -1;
}

bool io_expander_has_battery(void) { return expander_addr && is_ch32; }

int io_expander_battery_mv(void) {
    if (!io_expander_has_battery()) return -1;
    uint32_t total = 0;
    int n = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t b[2];
        if (iox_read_bytes(CH32_REG_ADC, b, 2)) {
            uint16_t raw = ((uint16_t)b[1] << 8) | b[0];
            total += raw > 1023 ? 1023 : raw;
            n++;
        }
    }
    if (n == 0) return -1;
    float v = (total / (float)n) * CH32_ADC_REF_V / 1023.0f * CH32_BAT_DIVIDER;
    return (int)(v * 1000.0f + 0.5f);
}
