# AMOLED-1.75C — `waveshare_amoled_175c`

Waveshare ESP32-S3-Touch-AMOLED-1.75C: 466×466 round CO5300, CST9217 touch,
AXP2101, 32 MB flash and 8 MB octal PSRAM. See `firmware/CLAUDE.md` for
shared rules.

- Vendor examples use CO5300 column offset 6, display reset GPIO 1, touch reset
  GPIO 2, I2C SDA 15 / SCL 14 and touch address 0x5A. Touch mirrors both axes.
- BOOT is the primary button; AXP2101 PKEY is PWR. The ES8311 uses MCLK 16,
  BCLK 9, LRCK 45, DOUT 8 and amplifier enable 46.
- The QMI8658 is fitted but unused; orientation is fixed. A serial-triggered
  chime at gain 0.7 and volume 70 was checked on hardware; the startup chime
  at that volume has not been checked.
