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


def test_landscape_environment_inherits_portrait_and_adds_board_flag() -> None:
    ini = (ROOT / "firmware/platformio.ini").read_text(encoding="utf-8")
    assert "[env:esp32_2432s024c_landscape]" in ini
    section = ini.split("[env:esp32_2432s024c_landscape]", 1)[1]
    assert "extends = env:esp32_2432s024c" in section
    assert "${env:esp32_2432s024c.build_flags}" in section
    assert "-DBOARD_LANDSCAPE" in section


def test_board_header_matches_verified_pins() -> None:
    header = (
        ROOT / "firmware/src/boards/esp32_2432s024c/board.h"
    ).read_text(encoding="utf-8")
    expected = {
        "LCD_NATIVE_WIDTH": "240",
        "LCD_NATIVE_HEIGHT": "320",
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


def test_board_header_separates_native_and_logical_dimensions() -> None:
    header = (
        ROOT / "firmware/src/boards/esp32_2432s024c/board.h"
    ).read_text(encoding="utf-8")
    assert "#define LCD_NATIVE_WIDTH 240" in header
    assert "#define LCD_NATIVE_HEIGHT 320" in header
    assert "#ifdef BOARD_LANDSCAPE" in header
    assert "#define LCD_WIDTH 320" in header
    assert "#define LCD_HEIGHT 240" in header
    assert "#define LCD_ROTATION 1" in header
    assert "#define LCD_ROTATION 0" in header


def test_board_hal_is_complete_and_uses_st7789() -> None:
    board_dir = ROOT / "firmware/src/boards/esp32_2432s024c"
    expected_sources = {
        "board_init.cpp",
        "caps.cpp",
        "display.cpp",
        "imu.cpp",
        "input.cpp",
        "power.cpp",
        "sound.cpp",
        "touch.cpp",
    }
    assert expected_sources <= {path.name for path in board_dir.glob("*.cpp")}

    display = (board_dir / "display.cpp").read_text(encoding="utf-8")
    assert "Adafruit_ST7789" in display
    assert "HSPI" in display
    assert "LCD_BACKLIGHT" in display
    assert "drawRGBBitmap" in display


def test_battery_ui_is_guarded_by_board_capability():
    ui = (ROOT / "firmware" / "src" / "ui.cpp").read_text(encoding="utf-8")
    assert "if (board_caps().has_battery)" in ui
    assert "init_battery_icons();" in ui
