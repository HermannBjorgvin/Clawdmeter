#pragma once

// Waveshare ESP32-S3-Touch-AMOLED-1.8 — portrait AMOLED kit.
// 368x448 SH8601 + FT3168 touch + AXP2101 PMU + QMI8658 IMU + XCA9554 expander.
// IMU is present (initialized for I2C bus health) but rotation is disabled
// because the panel mounts in a fixed orientation in the kit's enclosure.

#define BOARD_NAME           "Waveshare AMOLED 1.8"

// ---- Display geometry (portrait) ----
#define LCD_WIDTH            368
#define LCD_HEIGHT           448

// ---- QSPI display pins (SH8601) ----
#define LCD_CS               12
#define LCD_SCLK             11        // different from 2.16 board (was GPIO 38)
#define LCD_SDIO0            4
#define LCD_SDIO1            5
#define LCD_SDIO2            6
#define LCD_SDIO3            7
// LCD reset is routed through the XCA9554 IO expander (EXIO1). The Arduino
// GFX driver gets GFX_NOT_DEFINED; the expander releases reset before
// gfx->begin() runs.

// ---- I2C bus (touch + PMU + IMU + IO expander all share one bus) ----
#define IIC_SDA              15
#define IIC_SCL              14

// ---- Touch (minimal inline I2C reader; chip varies by board revision) ----
// Both controllers share the FocalTech-style data layout at regs 0x02..0x06,
// so only the I2C address differs. Revision is detected in board_init().
#define TP_INT               21
#define FT3168_ADDR          0x38   // original revision
#define CST816_ADDR          0x15   // later (CO5300) revision

// ---- PMU ----
#define AXP2101_ADDR         0x34

// ---- IO expander (XCA9554/PCA9554 compatible) ----
// Gates LCD_RST, TP_RST, audio amp enable, and reads the PWR button.
#define XCA9554_ADDR         0x20
#define IOX_PIN_TP_RST       0     // EXIO0 → touch reset (active LOW)
#define IOX_PIN_LCD_RST      1     // EXIO1 → display reset (active LOW)
#define IOX_PIN_PA_EN        2     // EXIO2 → audio amp enable
#define IOX_PIN_PWR_BTN      4     // EXIO4 → PWR button input, active HIGH

// ---- Audio (ES8311 mono codec + speaker, I2S) ----
// Same ES8311 + I2S path as the 2.16, different MCLK pin (16 vs 42). Amp enable
// is ambiguous across 1.8 hardware revisions: Waveshare's V2 example drives a
// plain GPIO 46, while this board's XCA9554 also exposes an amp-enable on EXIO2
// (IOX_PIN_PA_EN above). sound.cpp drives BOTH to be revision-safe. Pins from
// Waveshare's factory 15_ES8311 example. Untested on hardware in this repo.
#define SND_I2S_MCLK         16
#define SND_I2S_BCLK         9
#define SND_I2S_WS           45     // LRCK
#define SND_I2S_DOUT         8      // ESP → ES8311 (speaker)
#define SND_I2S_DIN          10     // ES8311 → ESP (mic; unused, set for STD mode)
#define SND_PA_PIN           46     // V2 direct-GPIO amp enable (HIGH = on)
#define SND_SAMPLE_RATE      44100  // must match the embedded PCM (bell_pcm.h)
#define SND_ES8311_ADDR      0x18

// ---- Buttons ----
#define BTN_BACK_GPIO        0     // BOOT — primary, Space (PTT)
// PWR comes via XCA9554 EXIO4 (see power.cpp); there is no secondary button.

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION         0
#define BOARD_HAS_IMU              1    // present + initialized, but rotation off
#define BOARD_HAS_BATTERY          1
#define BOARD_HAS_IO_EXPANDER      1
#define BOARD_HAS_SOUND            1
