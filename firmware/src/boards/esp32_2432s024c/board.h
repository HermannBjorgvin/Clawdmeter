#pragma once

#define BOARD_NAME "ESP32-2432S024C"

#define LCD_WIDTH 240
#define LCD_HEIGHT 320

#define LCD_SCLK 14
#define LCD_MOSI 13
#define LCD_MISO 12
#define LCD_CS 15
#define LCD_DC 2
#define LCD_RESET -1
#define LCD_BACKLIGHT 27

#define IIC_SDA 33
#define IIC_SCL 32

#define TP_INT -1
#define TP_RESET 25
#define TP_ADDR 0x15

#define BTN_PWR_GPIO 0

#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION 0
#define BOARD_HAS_IMU 0
#define BOARD_HAS_BATTERY 0
#define BOARD_HAS_IO_EXPANDER 0
