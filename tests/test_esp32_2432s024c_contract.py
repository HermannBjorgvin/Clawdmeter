from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_platformio_environment_targets_verified_board() -> None:
    ini = (ROOT / "firmware/platformio.ini").read_text(encoding="utf-8")
    assert "[env:esp32_2432s024c]" in ini
    section = ini.split("[env:esp32_2432s024c]", 1)[1]
    assert "board = esp32dev" in section
    assert "board_build.partitions = huge_app.csv" in section
    assert "+<boards/esp32_2432s024c/>" in section
    assert "-DBOARD_HAS_PSRAM" not in section.split("[env:", 1)[0]
    assert "lib_ignore =" in section
    assert "Adafruit seesaw Library" in section
    assert "SD" in section


def test_board_header_matches_verified_pins() -> None:
    header = (
        ROOT / "firmware/src/boards/esp32_2432s024c/board.h"
    ).read_text(encoding="utf-8")
    expected = {
        "LCD_WIDTH": "240",
        "LCD_HEIGHT": "320",
        "LCD_SCLK": "14",
        "LCD_MOSI": "13",
        "LCD_MISO": "12",
        "LCD_CS": "15",
        "LCD_DC": "2",
        "LCD_BACKLIGHT": "27",
        "IIC_SDA": "33",
        "IIC_SCL": "32",
        "TP_RESET": "25",
        "TP_ADDR": "0x15",
        "BTN_PWR_GPIO": "0",
    }
    for name, value in expected.items():
        assert f"#define {name}" in header
        assert value in header.split(f"#define {name}", 1)[1].splitlines()[0]
