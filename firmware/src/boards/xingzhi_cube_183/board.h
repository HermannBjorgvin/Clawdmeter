#pragma once

// Xingzhi Cube 1.83" TFT WiFi (2-mic variant) - ESP32-S3 (16 MB flash, 8 MB
// octal PSRAM), 284x240 landscape SPI TFT, three user buttons. No touch, no
// PMU, no IMU, no IO expander. Battery is a bare Li-Po with a charge-status pin
// and a voltage divider into an ADC pin; GPIO21 latches the power-hold circuit.
//
// The panel is sold as ST7789 but needs a vendor init table (the same one the
// board's ESPHome config uses: NV3023-style registers 0xFD..0xF6, colour order
// BGR, inversion on, swap_xy + mirror_y, 36-column offset). It is driven
// through Arduino_ST7789 with that table as its init operations.

#define BOARD_NAME           "Xingzhi Cube 1.83"

// ---- Display geometry (landscape) ----
#define LCD_WIDTH            284
#define LCD_HEIGHT           240

// ---- SPI display pins ----
#define LCD_SPI_CLK          9
#define LCD_SPI_MOSI         10
#define LCD_CS               14
#define LCD_DC               8
#define LCD_RST              18
#define LCD_BL               13    // LEDC PWM backlight

// ---- Buttons (active LOW, internal pull-ups) ----
#define BTN_BACK_GPIO        0     // BOOT   - primary, Space (PTT)
#define BTN_FWD_GPIO         40    // VOL    - secondary, Shift+Tab (mode toggle)
#define BTN_PWR_GPIO         39    // MID    - PWR role: brightness, hold-to-pair

// ---- Battery / power latch (pins and ADC calibration from PR #31) ----
// Standalone charger IC: CHRG goes HIGH while charging. Battery voltage comes
// through a divider into GPIO17 (ADC2_CH6; Clawdmeter uses BLE only, so ADC2
// stays usable). Drive PWR_LATCH_GPIO HIGH in board_init() to keep the board on
// when USB is removed.
#define BAT_ADC_GPIO         17
#define BAT_CHRG_GPIO        38
#define BAT_ADC_RAW_EMPTY    1970
#define BAT_ADC_RAW_FULL     2430
#define PWR_LATCH_GPIO       21

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 1
#define BOARD_HAS_ROTATION         0
#define BOARD_HAS_IMU              0
#define BOARD_HAS_BATTERY          1
#define BOARD_HAS_IO_EXPANDER      0
