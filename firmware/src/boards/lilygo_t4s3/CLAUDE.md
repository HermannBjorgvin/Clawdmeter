# T4-S3 — `lilygo_t4s3`

Hardware notes for this board. Shared firmware rules are in `firmware/CLAUDE.md`.

LilyGO T4-S3 (RM690B0 450×600 portrait AMOLED over QSPI, CST226SE touch @ 0x5A, SY6970 charger, ESP32-S3R8: 16 MB flash + 8 MB octal PSRAM). Build env: `lilygo_t4s3`.

- The RM690B0 addresses a 482-wide RAM and only the middle 450 columns are glass: every draw needs `LCD_COL_OFFSET` (16).
- `PMICEN` (GPIO 9) gates the panel/PMU rails and is driven HIGH in `board_init()`. GPIO 18 is the panel TE line: don't reuse it.
- The SY6970 is a charger, not a fuel gauge (`XPOWERS_CHIP_SY6970`): battery percent is approximated from the battery voltage (3.30–4.20 V).
- One user button, BOOT (GPIO 0 → Space/PTT). A hold past `PWR_LONGPRESS_MS` (1500 ms) synthesizes a PWR press (brightness), so there is no hold-to-pair gesture (`has_pwr_button = false`): re-pairing is the 5 s touch hold on the pairing hint.
- No IMU (fixed orientation), no audio.
