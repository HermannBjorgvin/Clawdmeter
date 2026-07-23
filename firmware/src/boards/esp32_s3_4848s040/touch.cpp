#include "../../hal/touch_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// Minimal GT911 reader. No external library — avoids the GPL Goodix dependency.
//
// Register map:
//   0x8140 (4 bytes) — Product ID ("911\0" when alive)
//   0x814E (1 byte)  — Status: bit7=buffer_ready, bits[3:0]=touch_count
//   0x8150 (8 bytes) — Point 1: trackId, x_l, x_h, y_l, y_h, sz_l, sz_h, reserved
// Write 0x00 to 0x814E after reading to clear the buffer-ready flag.
//
// I2C address is determined by the INT/RST pin state at power-on. With both
// pins unconnected (floating), 0x5D is the most common default; 0x14 is the
// alternative. We probe both at init and remember whichever responds.

static uint8_t  gt911_addr    = GT911_ADDR_PRIMARY;
static bool     gt911_ok      = false;
static bool     touch_pressed = false;
static uint16_t touch_x       = 0;
static uint16_t touch_y       = 0;

static bool gt911_read(uint16_t reg, uint8_t* buf, size_t len) {
    Wire.beginTransmission(gt911_addr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    // Use sendStop=true (stop-then-start) — the IDF 5.x i2c_master_ng driver
    // leaves the bus in a non-IDLE state after endTransmission(false).
    if (Wire.endTransmission(true) != 0) return false;
    if (Wire.requestFrom(gt911_addr, (uint8_t)len) != len) return false;
    for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
    return true;
}

static bool gt911_write(uint16_t reg, uint8_t val) {
    Wire.beginTransmission(gt911_addr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool gt911_probe(uint8_t addr) {
    gt911_addr = addr;
    uint8_t id[4] = {};
    if (!gt911_read(0x8140, id, 4)) return false;
    return id[0] == '9' && id[1] == '1' && id[2] == '1';
}

void touch_hal_init(void) {
    if (!gt911_probe(GT911_ADDR_PRIMARY) && !gt911_probe(GT911_ADDR_ALT)) {
        log_e("GT911", "not found on 0x%02X or 0x%02X", GT911_ADDR_PRIMARY, GT911_ADDR_ALT);
        return;
    }
    gt911_ok = true;
    gt911_write(0x814E, 0x00);  // clear any stale buffer-ready flag
    log_i("GT911", "init OK (addr=0x%02X)", gt911_addr);
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    if (!gt911_ok) { *pressed = false; return; }

    uint8_t status;
    if (!gt911_read(0x814E, &status, 1)) {
        *x = touch_x; *y = touch_y; *pressed = touch_pressed;
        return;
    }

    if (status & 0x80) {    // buffer_ready set: fresh data available
        uint8_t count = status & 0x0F;
        if (count > 0 && count <= 5) {
            uint8_t buf[8];
            if (gt911_read(0x8150, buf, 8)) {
                touch_x = (uint16_t)buf[1] | ((uint16_t)buf[2] << 8);
                touch_y = (uint16_t)buf[3] | ((uint16_t)buf[4] << 8);
                touch_pressed = true;
            }
        } else {
            touch_pressed = false;
        }
        gt911_write(0x814E, 0x00);  // release the buffer for next sample
    }

    *x = touch_x;
    *y = touch_y;
    *pressed = touch_pressed;
}
