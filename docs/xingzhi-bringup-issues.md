# Xingzhi Cube 1.83 Compatibility Notes

Support for the xingzhi-cube board was implemented with board-gated changes (`BOARD_XINGZHI_CUBE`) so existing Waveshare behavior stays intact.

Key requirements and fixes:
- ST7789 panel needs a vendor init table; base `Arduino_ST7789::tftInit()` was overridden to avoid SWRESET wiping vendor registers.
- Board config added for 284x240 SPI display, no touch/PMU/IMU, and 3 physical buttons.
- UI and splash renderer were made size-aware for 284x240 (layout constants, font scaling, dynamic splash canvas/cell sizing).
- BLE path hardened for cross-platform daemon compatibility (advertising retry/state handling and Windows-friendly daemon discovery matching).
- Splash color path updated for xingzhi with orange tinting, SPI byte-order handling, and black eye/detail preservation.

Validation: `xingzhi_cube_183` builds and flashes successfully; Usage, Bluetooth, and Splash screens render correctly on device.
