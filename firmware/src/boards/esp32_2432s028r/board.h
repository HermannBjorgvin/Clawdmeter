#pragma once

// ESP32-2432S028R — the "Cheap Yellow Display" (Sunton). Classic ESP32-D0WD
// (dual-core Xtensa, 4 MB flash, NO PSRAM), 2.8" 240x320 ILI9341 over 4-wire
// SPI, XPT2046 *resistive* touch on a second SPI bus, CH340 USB-UART.
//
// First resistive-touch port in the tree, and the second classic-ESP32 target.
// Everything the no-PSRAM C6 ports already handle applies here too: shrunk
// LVGL strips, direct-draw splash, static corner mascot, no screenshot path.
//
// Pin map from the board's published schematic and the community reference
// (Random Nerd Tutorials' ESP32-2432S028R pinout, which matches the
// witnessmenow/ESP32-Cheap-Yellow-Display TFT_eSPI setup header).
//
// NOTE ON REVISIONS: this model ships in several silicon revisions. The
// original (micro-USB, single USB port) is ILI9341. Later "R2"/USB-C units
// have been seen with ST7789 panels and, on some batches, a different init
// gamma. See PANEL/init notes in display.cpp — both are one build flag away.

#define BOARD_NAME           "ESP32-2432S028R (CYD)"

// ---- Display geometry ----
// The ILI9341 GRAM is 240x320 portrait; landscape is a controller rotation,
// not a different panel, so the native size stays 240x320 in both targets.
#define LCD_NATIVE_WIDTH     240
#define LCD_NATIVE_HEIGHT    320

#ifdef BOARD_LANDSCAPE
#define LCD_WIDTH            320
#define LCD_HEIGHT           240
#define LCD_ROTATION         1     // USB/CH340 edge on the left
#else
#define LCD_WIDTH            240
#define LCD_HEIGHT           320
#define LCD_ROTATION         0     // USB/CH340 edge at the bottom
#endif

// ---- Display SPI (ILI9341, 4-wire) — the ESP32's native HSPI pins ----
#define LCD_SCLK             14
#define LCD_MOSI             13
#define LCD_MISO             12    // wired but unused; the panel is write-only here
#define LCD_CS               15
#define LCD_DC               2
#define LCD_RST              -1    // no MCU reset line — panel RESX is tied to EN
#define LCD_BL               21    // backlight, LEDC PWM (TFT has no brightness cmd)
#define LCD_SPI_SPEED        40000000  // 40 MHz; 80 MHz is unreliable on CYD wiring

// ---- Touch (XPT2046, resistive) ----
// Its own SPI bus: these are NOT native VSPI pins, they route through the
// ESP32 GPIO matrix, which is why the bus is clocked slowly. The XPT2046 is
// spec'd well under 2.5 MHz anyway.
#define TP_CLK               25
#define TP_MOSI              32
#define TP_MISO              39    // input-only pin — fine for MISO
#define TP_CS                33
#define TP_IRQ               36    // input-only; LOW while the panel is pressed
#define TP_SPI_SPEED         2000000

// Resistive panels have no factory calibration — the controller returns raw
// 12-bit ADC counts whose usable range varies unit to unit. These are the
// community-standard CYD defaults; build with -DTOUCH_DEBUG to log raw
// samples over serial and retune for your panel (see touch.cpp).
#define TP_RAW_X_MIN         200
#define TP_RAW_X_MAX         3700
#define TP_RAW_Y_MIN         240
#define TP_RAW_Y_MAX         3800

// Touch-to-panel axis alignment. The XPT2046's X axis runs along the panel's
// *short* edge, so portrait needs no swap and landscape does. Flip these if
// your unit reads mirrored (see the QA notes in touch.cpp).
#ifdef BOARD_LANDSCAPE
#define TP_SWAP_XY           1
#define TP_INVERT_X          0
#define TP_INVERT_Y          1
#else
#define TP_SWAP_XY           0
#define TP_INVERT_X          0
#define TP_INVERT_Y          0
#endif

// Pressure gate. The XPT2046 reports a touch resistance; anything below this
// is noise or a release transient rather than a real finger.
#define TP_PRESSURE_MIN      400

// ---- Buttons ----
// The CYD has exactly one user button (BOOT on GPIO 0); the other is a
// hardware RST. It is wired to the PWR role rather than the HID primary:
// with an uncalibrated resistive panel the pairing gesture and brightness
// cycling have to stay reachable without touch. Cost: no HID Space (PTT).
#define BTN_PWR_GPIO         0
#define BTN_PWR_LONG_MS      1500

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION         0   // no IMU on this board
#define BOARD_HAS_IMU              0
#define BOARD_HAS_BATTERY          0   // USB-powered only, no battery circuit
#define BOARD_HAS_IO_EXPANDER      0
#define BOARD_HAS_SOUND            0   // GPIO 26 drives an amplified speaker, but
                                       // chime.cpp is an ES8311/I2S engine — see sound.cpp
