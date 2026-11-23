// The init lists and the ILI9342C/E probe below are adapted from M5GFX
// (https://github.com/m5stack/M5GFX): the lists from
// src/lgfx/v1/panel/Panel_ILI9342.hpp, which comes from LovyanGFX
// (https://github.com/lovyan03/LovyanGFX), and the probe from
// src/board_detect/m5/board_registry.inl. Their notices follow.
//
// ---- LovyanGFX ----
// Software License Agreement (FreeBSD License)
//
// Copyright (c) 2020 lovyan03 (https://github.com/lovyan03)
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// The views and conclusions contained in the software and documentation are those
// of the authors and should not be interpreted as representing official policies,
// either expressed or implied, of the FreeBSD Project.
//
// ---- M5GFX ----
// MIT License
//
// Copyright (c) 2021 M5Stack
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// ILI9342C / ILI9342E bring-up for M5Stack's Core-family panels (FIRE, Core2).
// Include from a board's display.cpp after board.h and <Arduino_GFX_Library.h>.
//
// Arduino_ILI9342's own init is Adafruit's generic ILI9341 list: it never
// unlocks the extended command set (C8h FF 93 42) and sends no gamma tables,
// so the panel runs on its power-on gamma and looks washed out. These lists
// are M5GFX's (Panel_ILI9342 and Panel_ILI9342E, m5stack/M5GFX, read
// 2026-11-23; notices above). Later Core2 units carry the ILI9342E, whose
// command set differs (unlock DDh, gamma E4h/E5h): the C list must not be
// sent to it.

enum Ili9342Variant : uint8_t { ILI9342_VARIANT_UNKNOWN, ILI9342_VARIANT_C, ILI9342_VARIANT_E };

// {command, data length, data...}
static const uint8_t ili9342c_init_list[] = {
    0xC8, 3, 0xFF, 0x93, 0x42,       // SETEXTC: unlock the extended commands
    0xC0, 2, 0x12, 0x12,             // power control 1
    0xC1, 1, 0x03,                   // power control 2
    0xC5, 1, 0xF2,                   // VCOM control 1
    0xB0, 1, 0xE0,                   // RGB interface signal control
    0xF6, 3, 0x01, 0x00, 0x00,       // interface control
    0xE0, 15, 0x00, 0x0C, 0x11, 0x04, 0x11, 0x08, 0x37, 0x89, 0x4C, 0x06, 0x0C, 0x0A, 0x2E, 0x34, 0x0F,  // positive gamma
    0xE1, 15, 0x00, 0x0B, 0x11, 0x05, 0x13, 0x09, 0x33, 0x67, 0x48, 0x07, 0x0E, 0x0B, 0x2E, 0x33, 0x0F,  // negative gamma
    0xB6, 4, 0x08, 0x82, 0x1D, 0x04, // display function control (240 lines)
};

static const uint8_t ili9342e_init_list[] = {
    0xDD, 1, 0x01,                   // unlock the extended commands
    0xD5, 1, 0x00,
    0xB1, 1, 0x22,
    0xC8, 1, 0x38,                   // power control 3 (VCOM)
    0xCB, 1, 0x1C,
    0xC9, 1, 0x1A,
    0xCA, 1, 0x1A,
    0xB7, 4, 0x5A, 0x41, 0x11, 0x19,
    0xE4, 15, 0x04, 0x08, 0x11, 0x06, 0x12, 0x07, 0x3A, 0x76, 0x47, 0x07, 0x0F, 0x0A, 0x11, 0x19, 0x05,  // positive gamma
    0xE5, 15, 0x02, 0x03, 0x07, 0x06, 0x12, 0x07, 0x36, 0x5F, 0x48, 0x06, 0x10, 0x0C, 0x16, 0x14, 0x09,  // negative gamma
};

class M5_ILI9342 : public Arduino_ILI9342 {
public:
    M5_ILI9342(Arduino_DataBus* bus, int8_t rst, uint8_t r, bool ips, Ili9342Variant v)
        : Arduino_ILI9342(bus, rst, r, ips), _variant(v) {}

protected:
    void tftInit() override {
        if (_rst != GFX_NOT_DEFINED) {
            pinMode(_rst, OUTPUT);
            digitalWrite(_rst, HIGH);
            delay(100);
            digitalWrite(_rst, LOW);
            delay(ILI9342_RST_DELAY);
            digitalWrite(_rst, HIGH);
            delay(ILI9342_RST_DELAY);
        } else {
            _bus->sendCommand(ILI9342_SWRESET);
            delay(ILI9342_RST_DELAY);
        }

        const bool e = _variant == ILI9342_VARIANT_E;
        const uint8_t* list = e ? ili9342e_init_list : ili9342c_init_list;
        const size_t len = e ? sizeof(ili9342e_init_list) : sizeof(ili9342c_init_list);
        _bus->beginWrite();
        for (size_t i = 0; i + 1 < len;) {
            _bus->writeCommand(list[i]);
            const uint8_t n = list[i + 1];
            for (uint8_t k = 0; k < n; ++k) _bus->write(list[i + 2 + k]);
            i += 2 + n;
        }
        _bus->writeCommand(0x3A);  // COLMOD: 16 bits per pixel
        _bus->write(0x55);
        _bus->writeCommand(ILI9342_SLPOUT);
        _bus->endWrite();
        delay(ILI9342_SLPOUT_DELAY);
        _bus->beginWrite();
        _bus->writeCommand(ILI9342_DISPON);
        _bus->endWrite();

        invertDisplay(false);  // applies the ips flag (INVON)
    }

private:
    Ili9342Variant _variant;
};

__attribute__((unused)) static const char* ili9342_variant_name(Ili9342Variant v) {
    switch (v) {
    case ILI9342_VARIANT_C: return "ili9342c";
    case ILI9342_VARIANT_E: return "ili9342e";
    default:                return "ili9342?";
    }
}

// ---- C / E probe (M5GFX board_detect's), bit-banged before the SPI bus starts ----
// D9h (0x10 | n) selects which parameter of the next read comes out. The panel
// answers on its bidirectional data line, i.e. MOSI (M5GFX reads there too;
// MISO stays low).
// The read is framed loosely: the 16 bits after the command are searched for
// both expected values (low 7 bits, as M5GFX compares them) at one offset.

static void ili_bb_byte(uint8_t v) {
    for (int i = 7; i >= 0; --i) {
        digitalWrite(LCD_MOSI, (v >> i) & 1);
        digitalWrite(LCD_SCLK, HIGH);
        delayMicroseconds(1);
        digitalWrite(LCD_SCLK, LOW);
        delayMicroseconds(1);
    }
}

static void ili_bb_write(uint8_t cmd, const uint8_t* d, int n) {
    digitalWrite(LCD_CS, LOW);
    digitalWrite(LCD_DC, LOW);
    ili_bb_byte(cmd);
    digitalWrite(LCD_DC, HIGH);
    for (int i = 0; i < n; ++i) ili_bb_byte(d[i]);
    digitalWrite(LCD_CS, HIGH);
    delayMicroseconds(2);
}

static void ili_bb_write1(uint8_t cmd, uint8_t d) { ili_bb_write(cmd, &d, 1); }

static uint16_t ili_bb_read(uint8_t cmd, uint8_t index) {
    ili_bb_write1(0xD9, 0x10 | index);
    digitalWrite(LCD_CS, LOW);
    digitalWrite(LCD_DC, LOW);
    ili_bb_byte(cmd);
    digitalWrite(LCD_DC, HIGH);
    pinMode(LCD_MOSI, INPUT);
    uint16_t v = 0;
    for (int i = 0; i < 16; ++i) {
        digitalWrite(LCD_SCLK, HIGH);
        delayMicroseconds(1);
        v = (uint16_t)((v << 1) | (digitalRead(LCD_MOSI) ? 1 : 0));
        digitalWrite(LCD_SCLK, LOW);
        delayMicroseconds(1);
    }
    digitalWrite(LCD_CS, HIGH);
    pinMode(LCD_MOSI, OUTPUT);
    delayMicroseconds(2);
    return v;
}

static bool ili_bb_match(uint16_t a, uint16_t b, uint8_t pa, uint8_t pb) {
    for (int sh = 9; sh >= 0; --sh) {
        if (((a >> sh) & 0x7F) == (pa & 0x7F) && ((b >> sh) & 0x7F) == (pb & 0x7F)) return true;
    }
    return false;
}

// Leaves the pins as plain GPIOs; Arduino_GFX's bus begin() takes them over.
__attribute__((unused)) static Ili9342Variant ili9342_probe(uint32_t poll_ms) {
    pinMode(LCD_CS, OUTPUT);
    digitalWrite(LCD_CS, HIGH);
    pinMode(LCD_DC, OUTPUT);
    pinMode(LCD_SCLK, OUTPUT);
    digitalWrite(LCD_SCLK, LOW);
    pinMode(LCD_MOSI, OUTPUT);

    const uint32_t start = millis();
    for (;;) {
        ili_bb_write1(0xD9, 0x00);
        ili_bb_write1(0xDD, 0x01);
        ili_bb_write1(0xCB, 0x1C);
        uint16_t dd = ili_bb_read(0xDD, 1);
        uint16_t cb = ili_bb_read(0xCB, 1);
        ili_bb_write1(0xD9, 0x00);
        if (ili_bb_match(dd, cb, 0x01, 0x1C)) {
            Serial.printf("[display] ILI9342 read-back DD:%04x CB:%04x -> ILI9342E\n", dd, cb);
            return ILI9342_VARIANT_E;
        }
        static const uint8_t unlock[] = {0xFF, 0x93, 0x42};
        ili_bb_write(0xC8, unlock, 3);
        uint16_t id2 = ili_bb_read(0xD3, 2);
        uint16_t id3 = ili_bb_read(0xD3, 3);
        ili_bb_write1(0xD9, 0x00);
        if (ili_bb_match(id2, id3, 0x93, 0x42)) {
            Serial.printf("[display] ILI9342 read-back ID4:%04x %04x -> ILI9342C\n", id2, id3);
            return ILI9342_VARIANT_C;
        }
        if (millis() - start >= poll_ms) {
            Serial.printf("[display] ILI9342 read-back DD:%04x CB:%04x ID4:%04x %04x -> unknown, C assumed\n",
                          dd, cb, id2, id3);
            return ILI9342_VARIANT_UNKNOWN;
        }
        delay(1);
    }
}
