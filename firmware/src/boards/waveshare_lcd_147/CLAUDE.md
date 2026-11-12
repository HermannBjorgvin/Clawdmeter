# LCD-1.47 — `waveshare_lcd_147`

Hardware notes for this board. Shared firmware rules are in `firmware/CLAUDE.md`.

Non-AMOLED kit: a 172×320 **JD9853** IPS panel on plain 4-wire SPI, run rotated so the UI gets a **320×172 landscape** canvas. Pin map taken from the official schematic, not the wiki — Waveshare's own ESP-IDF BSP header has TP_RST/TP_INT swapped relative to the board.
- Display: JD9853 via SPI (CS=21, SCLK=38, MOSI=39, DC=45, RST=40, BL=46). Driven by Arduino_GFX's `Arduino_ST7789` (the JD9853 shares the ST7789 command set for CASET/RASET/RAMWR/MADCTL) **plus the vendor register sequence** in `jd9853_reg_init()`, pushed after `gfx->begin()`. Without that sequence the panel stays dark. Constructed portrait (172×320) with `col_offset1 = 34` — the visible area starts at column 34 of the controller's 240-wide GRAM — then `setRotation(1)`; `Arduino_TFT::setRotation` maps that offset onto the correct axis. Inversion comes from the vendor sequence's closing `INVON` (0x21), which overrides whatever `begin()` sent; `ips` only changes how `invertDisplay()` reads its argument.
- Brightness: LEDC PWM on the backlight GPIO (no panel brightness command), like the 1.54.
- Touch: **AXS5106L** @ I2C 0x63 (SDA=42, SCL=41, INT=48, RST=47), vendored inline reader — 14-byte burst from reg 0x01, finger count in byte 1. Needs a long reset pulse (200 ms low / 300 ms settle). Landscape maps raw portrait coords by swapping axes with no mirroring.
- Battery: VBAT → 3.0× divider → GPIO12 (**ADC2**_CH1). No PMU, so charging/VBUS state is unknowable and reports false. ADC2 is only contended by Wi-Fi, which this firmware never starts.
- **No cell can't be detected reliably.** On USB with no battery, VBAT is whatever the charger leaves on the pin, and it differs between units of the same kit: of two tested boards, one reads an empty battery and the other a full one. Only a reading under 3.0 V counts as "no battery". With a cell attached the reading is correct.
- Buttons: **BOOT (GPIO 0) is the only readable key** and it takes the PWR role (brightness / hold-3s-release pairing) — without it the device could never be paired. So `BoardCaps.button_count == 0` and the HID Space / Shift+Tab paths never fire on this board.
- **No hold-to-power-off** (unlike the 1.54): there is no power-hold latch to drop, and GPIO 0 is the S3's boot strapping pin — an ext0 LOW-level wake would sample it while still held and drop the chip into USB download mode.
- No IO expander, no IMU, no codec/buzzer. 16 MB flash + 8 MB octal PSRAM (ESP32-S3R8).
- UI: too short to stack the usage cards, so `layout_compute()` puts them side by side (the landscape-strip rule in `layout.cpp`).
