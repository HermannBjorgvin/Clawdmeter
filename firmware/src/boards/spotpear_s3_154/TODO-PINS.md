# SpotPear ESP32-S3-LCD-1.54-MUMA — verified pinout & notes

This board is the **"Spotpear ESP32-S3-LCD-1.54-MUMA"** (a XiaoZhi "muma" AI
toy, 1.54" ST7789 240×240, N16R8). Its pinout does **NOT** match Waveshare's
`ESP32-S3-Touch-LCD-1.54` despite SpotPear's wiki linking to that repo.

Pins verified two ways: (1) the xiaozhi-esp32 board config
`main/boards/sp-esp32-s3-1.54-muma/config.h` (github.com/78/xiaozhi-esp32),
and (2) an on-hardware I2C scan that independently found the touch bus.

Status: **display + touch + BLE all verified working on hardware.** Splash
renders in correct Claude-orange (confirmed by framebuffer screenshot).

## Confirmed pinout

| Signal            | GPIO | Notes |
|-------------------|:----:|-------|
| LCD SCLK          |  4   | `Arduino_ESP32SPI` |
| LCD MOSI          |  2   | |
| LCD CS            |  5   | |
| LCD DC            | 47   | |
| LCD RST           | 38   | |
| LCD backlight     | 42   | **ACTIVE-LOW** PWM (`LCD_BL_INVERT`) |
| LCD SPI clock     | —    | 40 MHz (`LCD_SPI_HZ`) |
| Touch I2C SDA     | 11   | CST816 |
| Touch I2C SCL     |  7   | |
| Touch INT         | 12   | |
| Touch RST         |  6   | |
| Touch addr        | 0x15 | CST816 (chip ID reg 0xA7 → 0xB6) |
| BOOT button       |  0   | primary (Space / PTT) |
| (RST/"PWR" button)|  EN  | hardware reset, not a GPIO |

Panel: ST7789, RGB element order, color-inversion ON (Arduino_GFX `IPS=true`
sends INVON). Display 240×240, no offset, no mirror, no swap.

Not populated / not wired on this board: PMU, IMU, audio buzzer → `power.cpp`,
`imu.cpp`, `sound.cpp` are stubs (`BOARD_HAS_BATTERY/IMU/SOUND=0`). There is a
battery-charge detect on GPIO 41 and a charge LED on GPIO 3 if ever needed.

## Flashing this board from WSL (important)

The ESP32-S3 uses native USB-Serial-JTAG, which **re-enumerates on every chip
reset**, so a bare `usbipd attach` drops mid-flash. Use auto-attach and a udev
rule so the port survives resets and stays accessible:

```powershell
# Windows admin PowerShell (leave running):
usbipd attach --wsl --busid <BUSID> --auto-attach
```
```bash
# WSL, once: make any Espressif port world-accessible
echo 'SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", MODE="0666"' | sudo tee /etc/udev/rules.d/99-esp32.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

PlatformIO runs from a venv at `~/.pio-venv` (system pip was a broken msys2
shim). Build/flash:
```bash
~/.pio-venv/bin/pio run -d firmware -e spotpear_s3_154 -t upload --upload-port /dev/ttyACM0
```

Screenshot (framebuffer QA over serial, needs Pillow in the venv):
`LV_USE_SNAPSHOT=1` is enabled for this env; send `screenshot\n` at 115200 and
read back `SCREENSHOT_START w h size` + raw little-endian RGB565.

## If a future unit misbehaves

- Colors look negative → drop `IPS`/INVON (flip the `true` in the
  `Arduino_ST7789(...)` ctor) or `gfx->invertDisplay(false)`.
- Image mirrored/rotated → adjust the rotation arg (3rd param) of
  `Arduino_ST7789(...)`.
- Touch X/Y swapped/mirrored → fix in `touch.cpp` (config has MIRROR/SWAP all
  false, so direct mapping is expected).
