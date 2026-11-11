# uPesy WROOM + ST7789 1.9" — `upesy_wroom_st7789`

Hardware notes for this board. Shared firmware rules are in `firmware/CLAUDE.md`.

uPesy ESP32 WROOM DevKit (classic ESP32-D0WD, 4 MB flash, **no PSRAM**, CH340 USB-UART) hand-wired to a bare 1.9" 170×320 ST7789 SPI module. Portrait 170×320 only. Landscape 320×170 was tried: the panel driver is fine at rotation 3, but the shared layout needs about 198 px for its two stacked cards, so card 2 clips and the status line overlaps it. Landscape needs a short-and-wide shape rule in `layout.cpp` first.

- Built from the CYD port (`esp32_2432s028r`): same SoC, no-PSRAM memory limits, `huge_app` partitions, BOOT-as-PWR button, no touch/battery/IMU/sound.
- Wiring: SCLK 18, MOSI 23 (native VSPI), CS 15, DC 2, RST 4, backlight 32 (LEDC PWM). GPIO 2 and 15 are strapping pins. On many ESP32 devkits GPIO 2 also drives the on-board LED; if yours does, the LED flickers with DC during flushes (harmless), or move DC to a free pin (16, 17, 21) and update `board.h`.
- **Flashing needs BOOT held by hand**: auto-reset fails with "Wrong boot mode (0x13)": `boot:0x13` decodes to GPIO 0 high, GPIO 2 low and GPIO 15 high, so the DTR/RTS auto-reset did not pull GPIO 0 low on this unit. DC and CS on strapping pins 2 and 15 are not the cause. Hold BOOT, tap EN, then upload.
- The panel shows the middle 170 of the ST7789's 240 columns: `LCD_COL_OFFSET` 35, as on the T-Display S3. Verified on hardware at rotations 3, 0 and 2 (no edge strip). The env uses rotation 2: portrait, upside down. A noise strip along one long edge means the offset is wrong.
- Inversion defaults ON, colours verified correct on hardware. If a different module comes out photo-negative, build with `-DST7789_NO_INVERT`.
- No touch controller, so `has_touch = false` and `touch.cpp` reports nothing.
