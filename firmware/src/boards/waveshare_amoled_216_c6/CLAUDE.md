# AMOLED-2.16 (C6) — `waveshare_amoled_216_c6`

Hardware notes for this board. Shared firmware rules are in `firmware/CLAUDE.md`.

Waveshare ESP32-C6-Touch-AMOLED-2.16 (CO5300, 480×480, CST9217 touch). Build env: `waveshare_amoled_216_c6`. ESP32-C6 SoC: single-core RISC-V, **no PSRAM**, BLE 5 only.

## Audio

- ES8311 codec on the shared I2C bus (0x18), I2S MCLK 19, BCLK 20, WS 22, DOUT 23, DIN 21 (from Waveshare's `07_Audio_Test`).
- No amp-enable line: Waveshare's BSP sets `BSP_POWER_AMP_IO` to `GPIO_NUM_NC`, so `sound.cpp` passes a null `amp_enable` and `chime.cpp` keeps the ES8311 DAC muted between dings. Don't hunt for a PA pin on the TCA9554.
- No PSRAM, so the ding buffer comes from internal RAM, once, after setup. The whole ding needs the largest free internal block to be at least `CHIME_RAM_RESERVE` (48 KB) + 2 × `ding_frames(SND_SAMPLE_RATE)` bytes: ~85 KB at 11025 Hz. Less and the boot log's `room for N of 18411 frames` drops and the ding is cut short with a fade; below 0.5 s of room it plays nothing and every ding logs `chime: no ding buffer`. The C6 has no FPU, so there's no live-synthesis fallback.
- The `i2cWrite ... ESP_ERR_INVALID_STATE` line during `imu_hal_init` is SensorLib polling the QMI8658 mid-reset (lewisxhe/SensorLib#46). It's harmless and unrelated to audio.
