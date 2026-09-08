---
name: firmware-build
description: "Use when compiling, sizing, flashing, or diagnosing the ESP32 firmware with PlatformIO."
---

# Firmware Build

Use the repository environment and prefer:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -d firmware
```

For upload, add `-t upload --upload-port <PORT>`. Record the board environment, compiler errors, warnings, flash usage, and RAM usage. Do not change the platform or partition scheme to hide a size failure. The pioarduino platform is required by the current GFX/LVGL setup.
