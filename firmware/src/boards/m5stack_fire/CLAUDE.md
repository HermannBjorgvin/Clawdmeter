# M5Stack FIRE — `m5stack_fire`

Hardware notes for this board. Shared firmware rules are in `firmware/CLAUDE.md`.

M5Stack FIRE (ILI9342C 320×240 landscape over 4-wire SPI on VSPI, 16 MB flash + PSRAM on a WROVER module). Build env: `m5stack_fire`. **The first classic ESP32 port** (every other board is S3 or C6): USB is a CH9102 UART bridge with no native USB, so Serial is plain UART0 and the env must **not** set `ARDUINO_USB_CDC_ON_BOOT`.

- Pins are the canonical M5Stack Core/Fire wiring. Backlight is LEDC PWM on GPIO 32; the panel has no brightness command.
- **No touch screen.** Three front buttons on input-only GPIOs 39/38/37 with external pull-ups (so plain `INPUT`, not `INPUT_PULLUP`): A (left) → Space/PTT, C (right) → Shift+Tab, B (middle) → PWR role (short → brightness, hold ~1.5 s and release → pair). With no touchscreen, a press of A first dismisses an active alert (`director_button`), then keeps its PTT role.
- Battery and charging come from the IP5306 power-bank IC over I2C (0x75, internal bus SDA 21 / SCL 22): a coarse 4-LED gauge (25/50/75/100 %). Hardware power on/off is the IP5306's own red side button; the firmware never synthesizes a software power-off.
- MPU6886 IMU populated but unused (fixed orientation). The speaker is a DAC path, not the I2S chime engine: no sound.
