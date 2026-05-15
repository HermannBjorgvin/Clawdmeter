#pragma once
//
// Board: Waveshare ESP32-S3-Touch-LCD-2
// https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-2
//
// 240x320 ST7789T3 IPS LCD via SPI, CST816D capacitive touch (I2C),
// QMI8658C accelerometer (same I2C bus as touch), ADC battery sense on
// GPIO5, ETA-class linear charger (no AXP2101 / PMU). SD card and OV3660
// camera are wired but unused for the dashboard build.
//
// ESP32-S3R8, 16 MB flash, 8 MB PSRAM.
//

// ---- Display ----
#define BOARD_NAME              "Waveshare ESP32-S3-Touch-LCD-2"
#define BOARD_LCD_W             240
#define BOARD_LCD_H             320
#define BOARD_LCD_IS_AMOLED     0     // backlit IPS, no DCS brightness
#define BOARD_LCD_IS_ROUND      0
#ifndef BOARD_FIXED_ROTATION
#define BOARD_FIXED_ROTATION    0
#endif

// ST7789T3 SPI pins. LCD_RESET shares GPIO0 with the BOOT/STRAP signal
// — gfx->begin() pulses it low to reset the panel, which also briefly
// pulls GPIO0 low. After init GPIO0 is held high, so the BOOT button is
// usable as an emergency reset only; do not map it as a normal nav
// button or you'll reset the LCD on every press.
#define LCD_MOSI                38
#define LCD_SCLK                39
#define LCD_DC                  42
#define LCD_CS                  45
#define LCD_RESET               0
#define LCD_BL                  1
#define LCD_MISO                -1     // not connected
#define LCD_COL_OFFSET          0      // standard 240x320 ST7789
#define LCD_ROW_OFFSET          0

// ---- I2C (shared by touch + IMU) ----
#define IIC_SDA                 48
#define IIC_SCL                 47

// ---- Touch (CST816D) ----
#define BOARD_HAS_TOUCH         1
// Unified SensorLib CST driver. It auto-detects the actual chip — the
// CST816D in our case — based on the I2C address, so the same type
// is reused on the Waveshare AMOLED (CST9220 @ 0x5A).
#define BOARD_TOUCH_CLASS       TouchDrvCSTXXX
#define BOARD_TOUCH_ADDR        0x15      // factory default for CST816 family
#define TP_INT                  46
#define TP_RST                  -1        // not wired; chip self-resets at POR

// ---- PMU ----
// ETA6098-class linear charger, no PMU IC. Battery state read from ADC.
#define BOARD_HAS_PMU           0
#define BOARD_HAS_PWR_BUTTON    0
#define BOARD_HAS_BATTERY_ADC   1

// Battery ADC. Schematic shows R19=200K / R20=100K, so V_bat is divided
// by 3 before reaching GPIO5. Correction factor is left at 1.0 until
// the boot-time raw log is used to calibrate.
#define BAT_ADC_GPIO            5
#define BAT_ADC_MAX             4095
#define BAT_ADC_REF_VOLTAGE     3.3f
#define BAT_ADC_DIVIDER_RATIO   3.0f
#define BAT_ADC_CORRECTION_FACTOR 1.0f
#define BAT_CHARGE_DETECT_USB_CDC 1  // Charger status pin is not exposed.

// ---- IMU (QMI8658C, optional auto-rotation) ----
// Hooked to the same I2C bus as the touch. Default rotation tracking is
// disabled until the firmware is observed to work fine on this board —
// flip BOARD_FIXED_ROTATION above and set this flag to 1 to opt in.
#define BOARD_HAS_IMU           0
#define IMU_INT1                3

// ---- Physical buttons ----
// KEY1 is wired to ESP_EN (hardware reset, not an application input).
// KEY2 is BOOT/GPIO0 which doubles as LCD_RESET — unsafe to map as a
// nav button. Navigation on this board is done entirely through touch.
#define BOARD_BTN_COUNT         0
#define BOARD_BTN_ACTIVE_LOW    1

// ---- Storage / camera ----
// Documented for reference; the dashboard build does not initialize
// either subsystem.
#define SD_MOSI                 38
#define SD_SCLK                 39
#define SD_MISO                 40
#define SD_CS                   41

#define CAM_PWDN                17
#define CAM_XCLK                8
#define CAM_PCLK                9
#define CAM_VSYNC               6
#define CAM_HREF                4
#define CAM_SDA                 21
#define CAM_SCL                 16
