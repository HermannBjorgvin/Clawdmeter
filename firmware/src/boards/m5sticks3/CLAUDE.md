# M5StickS3 — `m5sticks3`

Hardware notes for this board. Shared firmware rules are in `firmware/CLAUDE.md`.

M5Stack M5StickS3 (ST7789P3 135×240 portrait over 4-wire SPI, ESP32-S3-PICO-1-N8R8: 8 MB quad flash + 8 MB octal PSRAM). Build env: `m5sticks3`.

- **Power, battery, the LCD rail and the speaker amp all hang off the M5PM1 PMIC** (I2C 0x6E on the internal bus, SDA 47 / SCL 48; register access in `pm1.{h,cpp}`). `board_init()` wakes it (no idle sleep, watchdog off) and raises **PM1 GPIO2, the LCD rail**, before the panel is touched, or the panel stays dark. PM1 GPIO3 gates the AW8737 amp. The PMIC's PEK button does hard power in hardware (click on, double-click off) and is not an ESP32 GPIO.
- Backlight is LEDC PWM on GPIO 38 (active high); the panel has no brightness command.
- **No touchscreen.** BtnA (GPIO 11, front) → Space/PTT; BtnB (GPIO 12, side) → PWR role (short → brightness, hold ~1.5 s and release → pair). With no touchscreen, a BtnA press first dismisses an active alert (`director_button`), then keeps its PTT role.
- ES8311 + AW8737 chime over I2S (MCLK 18, BCLK 17, WS 15, DOUT 14). BMI270 IMU populated but unused (fixed orientation).
- The 135-px width is the layout's narrow case (`fit_narrow()` in `layout.cpp`): short pill labels and a wrapping pairing hint.
