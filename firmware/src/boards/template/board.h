#pragma once

// Template board.h — copy this file (and the rest of boards/template/) to
// boards/<your_board>/ and fill in each TODO. See docs/porting/adding-a-board.md
// for a walk-through.

#define BOARD_NAME           "TODO: human-readable board name"

// ---- Display geometry ----
// Active panel pixel dimensions (post-orientation).
#define LCD_WIDTH            240   // TODO
#define LCD_HEIGHT           240   // TODO

// ---- QSPI display pins ----
// Wire your panel datasheet's QSPI pins to MCU GPIOs and list them here.
#define LCD_CS               12    // TODO
#define LCD_SCLK             38    // TODO
#define LCD_SDIO0            4     // TODO
#define LCD_SDIO1            5     // TODO
#define LCD_SDIO2            6     // TODO
#define LCD_SDIO3            7     // TODO
#define LCD_RESET            2     // TODO; use GFX_NOT_DEFINED if you reset via an expander

// ---- I2C bus (shared by touch, PMU, IMU, IO expander) ----
#define IIC_SDA              15    // TODO
#define IIC_SCL              14    // TODO

// ---- Touch ----
// I2C address depends on the controller. Replace TODO_TP_ADDR below and pick
// the matching driver in touch.cpp.
#define TP_INT               11    // TODO
#define TP_ADDR              0x00  // TODO

// ---- PMU ----
// Drop or change if your board doesn't ship an AXP2101.
#define AXP2101_ADDR         0x34

// ---- Buttons ----
#define BTN_BACK_GPIO        0     // BOOT — primary, Space (PTT)
// If your board has a second physical button, set its GPIO and bump
// BOARD_HAS_SECONDARY_BUTTON to 1 below.

// ---- Capability flags ----
// Compile-time switches the linker uses to dead-strip optional features.
// Keep these in sync with the BoardCaps instance in caps.cpp.
#define BOARD_HAS_SECONDARY_BUTTON 0   // TODO
#define BOARD_HAS_ROTATION         0   // TODO: IMU-driven CPU rotation in display.cpp
#define BOARD_HAS_IMU              0   // TODO
#define BOARD_HAS_BATTERY          0   // TODO
#define BOARD_HAS_IO_EXPANDER      0   // TODO
