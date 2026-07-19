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
    assert "#define LCD_ROTATION 3" in header
    assert "#define LCD_ROTATION 0" in header


def test_usb_left_landscape_rotation_color_contract() -> None:
    board_dir = ROOT / "firmware/src/boards/esp32_2432s024c"
    color = (board_dir / "display_color_order.h").read_text(encoding="utf-8")
    normalized_color = " ".join(color.split())

    assert "rotation == 3 ? 0xE8 : 0x88" in normalized_color


def test_usb_left_landscape_touch_contract() -> None:
    touch = (
        ROOT / "firmware/src/boards/esp32_2432s024c/touch_mapping.h"
    ).read_text(encoding="utf-8")
    normalized_touch = " ".join(touch.split())

    assert "static_cast<uint16_t>(319 - y)" in normalized_touch
    assert "static_cast<uint16_t>(239 - x)" in normalized_touch


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


def test_landscape_claude_usage_uses_three_compact_rows() -> None:
    ui = (ROOT / "firmware" / "src" / "ui.cpp").read_text(encoding="utf-8")
    usage = ui.split("static void init_usage_screen", 1)[1].split(
        "// ======== Public API", 1
    )[0]
    normalized = " ".join(usage.split())
    assert "if (L.claude_compact_rows)" in normalized
    assert normalized.count("make_compact_usage_row(") == 3
    for label in ("Currently", "Weekly", "Fable"):
        assert f'"{label}"' in normalized


def test_landscape_claude_footer_is_compact_next_to_page_dots() -> None:
    ui = (ROOT / "firmware" / "src" / "ui.cpp").read_text(encoding="utf-8")
    normalized = " ".join(ui.split())

    assert "lv_obj_set_width(lbl_anim, L.claude_status_w);" in normalized
    assert "lv_obj_set_pos(lbl_anim, L.claude_status_x, L.claude_status_y);" in normalized
    assert "lv_obj_set_style_text_font(lbl_anim, &font_styrene_14, 0);" in normalized
    assert "if (L.claude_compact_rows) { lv_label_set_text(lbl_anim, text);" in normalized


def test_landscape_codex_uses_second_card_coordinates() -> None:
    ui = (ROOT / "firmware" / "src" / "ui.cpp").read_text(encoding="utf-8")
    codex = ui.split("codex_container = lv_obj_create", 1)[1].split(
        "activity_container = lv_obj_create", 1
    )[0]
    normalized = " ".join(codex.split())
    assert "const int second_x = L.horizontal_cards ? L.second_panel_x : L.margin;" in normalized
    assert "const int second_y = L.horizontal_cards ? L.content_y :" in normalized
    assert "make_usage_panel(codex_container, second_x, second_y, L.panel_width," in normalized


def test_landscape_activity_uses_second_card_coordinates() -> None:
    ui = (ROOT / "firmware" / "src" / "ui.cpp").read_text(encoding="utf-8")
    activity = ui.split("activity_container = lv_obj_create", 1)[1].split(
        "activity_footer_label = lv_label_create", 1
    )[0]
    normalized = " ".join(activity.split())
    assert "activity_container, L.margin, L.content_y, L.panel_width" in normalized
    assert "activity_container, second_x, second_y, L.panel_width" in normalized


def test_provider_logos_use_top_left_pivot_before_scaling_and_positioning() -> None:
    ui = (ROOT / "firmware" / "src" / "ui.cpp").read_text(encoding="utf-8")
    start = ui.index("claude_logo_img = lv_image_create")
    end = ui.index("if (board_caps().has_battery)", start)
    logo = ui[start:end]
    for provider in ("claude", "codex"):
        provider_logo = logo.split(
            f"{provider}_logo_img = lv_image_create", 1
        )[1]
        if provider == "claude":
            provider_logo = provider_logo.split(
                "codex_logo_img = lv_image_create", 1
            )[0]
        source_index = provider_logo.index(
            f"lv_image_set_src({provider}_logo_img, &{provider}_logo_dsc);"
        )
        pivot_index = provider_logo.index(
            f"lv_image_set_pivot({provider}_logo_img, 0, 0);"
        )
        scale_index = provider_logo.index(
            f"lv_image_set_scale({provider}_logo_img, L.logo_scale);"
        )
        position_index = provider_logo.index("lv_obj_set_pos(")
        assert source_index < pivot_index < scale_index < position_index


def test_codex_logo_is_80x80_rgb565a8() -> None:
    header = (ROOT / "firmware" / "src" / "codex_logo.h").read_text(
        encoding="utf-8"
    )
    assert "#define CODEX_LOGO_WIDTH 80" in header
    assert "#define CODEX_LOGO_HEIGHT 80" in header
    assert "static const uint8_t codex_logo_data[19200]" in header
    payload = header.split("codex_logo_data[19200] = {", 1)[1].split("};", 1)[0]
    assert payload.count("0x") == 19200


def test_ui_owns_separate_claude_and_codex_logo_images() -> None:
    ui = (ROOT / "firmware" / "src" / "ui.cpp").read_text(encoding="utf-8")
    assert "static lv_obj_t* claude_logo_img;" in ui
    assert "static lv_obj_t* codex_logo_img;" in ui
    assert "static lv_image_dsc_t claude_logo_dsc;" in ui
    assert "static lv_image_dsc_t codex_logo_dsc;" in ui


def test_activity_battery_visibility_uses_page_overlap_policy() -> None:
    ui = (ROOT / "firmware" / "src" / "ui.cpp").read_text(encoding="utf-8")
    battery = ui.split("static void apply_battery_visibility", 1)[1].split(
        "static void apply_brand_visibility", 1
    )[0]
    assert "dashboard_battery_visible(dashboard_visibility, current_page)" in battery


def test_battery_updates_follow_splash_and_dashboard_state_sequence() -> None:
    ui = (ROOT / "firmware" / "src" / "ui.cpp").read_text(encoding="utf-8")
    assert "static DashboardVisibilityState dashboard_visibility = {};" in ui

    boot_splash = ui.split("void ui_show_boot_splash(void) {", 1)[1].split(
        "\n}", 1
    )[0]
    boot_transition = boot_splash.index(
        "dashboard_visibility_show_boot_splash(dashboard_visibility);"
    )
    boot_battery_hide = boot_splash.index(
        "lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);"
    )
    assert boot_transition < boot_battery_hide

    show_screen = ui.split("void ui_show_screen(DashboardPage page) {", 1)[1].split(
        "void ui_start_dashboard", 1
    )[0]
    dashboard_transition = show_screen.index(
        "dashboard_visibility_show_dashboard(dashboard_visibility);"
    )
    policy_apply = show_screen.index("apply_battery_visibility();")
    assert dashboard_transition < policy_apply

    update_battery = ui.split("void ui_update_battery(int percent, bool charging) {", 1)[
        1
    ].split("\n}", 1)[0]
    assert update_battery.index("lv_image_set_src") < update_battery.index(
        "apply_battery_visibility();"
    )


def test_dashboard_carousel_has_no_robot_page() -> None:
    header = (ROOT / "firmware" / "src" / "dashboard_carousel.h").read_text(
        encoding="utf-8"
    )
    assert "DASHBOARD_ROBOT" not in header


def test_ui_has_no_robot_status_or_transport_freshness_copy() -> None:
    ui = (ROOT / "firmware" / "src" / "ui.cpp").read_text(encoding="utf-8")
    assert "robot_status_label" not in ui
    assert "last_received_update_ms" not in ui
    assert "last_robot_status_refresh_ms" not in ui
    assert "last_received_transport" not in ui
    assert '"USB data"' not in ui
    assert '"BLE data"' not in ui


def test_global_click_routes_left_and_right_after_reading_touch_point() -> None:
    ui = (ROOT / "firmware" / "src" / "ui.cpp").read_text(encoding="utf-8")
    callback = ui.split("static void global_click_cb(lv_event_t* e) {", 1)[1].split(
        "\n}", 1
    )[0]

    active_index = callback.index("lv_indev_active()")
    point_index = callback.index("lv_indev_get_point")
    previous_index = callback.index("carousel_manual_previous")
    next_index = callback.index("carousel_manual_next")

    assert active_index < point_index
    assert point_index < previous_index
    assert point_index < next_index


def test_boot_splash_stays_outside_dashboard_carousel() -> None:
    main = (ROOT / "firmware" / "src" / "main.cpp").read_text(encoding="utf-8")
    ui_header = (ROOT / "firmware" / "src" / "ui.h").read_text(encoding="utf-8")
    ui = (ROOT / "firmware" / "src" / "ui.cpp").read_text(encoding="utf-8")

    assert "DASHBOARD_ROBOT" not in main
    assert "ui_show_boot_splash();" in main
    assert "splash_is_active()" in main
    assert "void ui_show_boot_splash(void);" in ui_header

    boot_splash = ui.split("void ui_show_boot_splash(void) {", 1)[1].split(
        "\n}", 1
    )[0]
    assert "splash_show();" in boot_splash
    assert "lv_obj_add_flag(navigation_layer, LV_OBJ_FLAG_HIDDEN);" in boot_splash


def test_landscape_enterprise_copy_and_labels_are_bounded() -> None:
    ui = (ROOT / "firmware" / "src" / "ui.cpp").read_text(encoding="utf-8")
    assert 'L.horizontal_cards ? "monthly budget" : "of your monthly budget"' in ui
    assert "lv_obj_set_pos(lbl_spending_desc, 0, L.usage_description_y);" in ui
    assert "lv_obj_set_pos(lbl_spending_status, 0, L.usage_status_y);" in ui
    assert "lv_obj_set_width(lbl_spending_desc, usage_content_width);" in ui
    assert "lv_obj_set_width(lbl_spending_status, usage_content_width);" in ui
    assert "lv_label_set_long_mode(lbl_spending_desc, LV_LABEL_LONG_DOT);" in ui
    assert "lv_label_set_long_mode(lbl_spending_status, LV_LABEL_LONG_DOT);" in ui
    assert "lv_label_set_text(lbl_spending_status, pace_text);" in ui
    assert 'snprintf(buf, sizeof(buf), "Resets %s", data->reset_date);' in ui


def test_landscape_usage_labels_have_explicit_width_and_long_mode() -> None:
    ui = (ROOT / "firmware" / "src" / "ui.cpp").read_text(encoding="utf-8")
    assert "lv_obj_set_width(*out_pill, content_width);" in ui
    assert "lv_label_set_long_mode(*out_pill, LV_LABEL_LONG_DOT);" in ui
    assert "lv_obj_set_width(*out_reset, content_width);" in ui
    assert "lv_label_set_long_mode(*out_reset, LV_LABEL_LONG_DOT);" in ui


def test_landscape_pairing_uses_profile_offsets() -> None:
    ui = (ROOT / "firmware" / "src" / "ui.cpp").read_text(encoding="utf-8")
    assert "lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, L.pairing_title_y);" in ui
    assert "lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, L.pairing_instruction_y);" in ui
    assert "lv_obj_align(l3, LV_ALIGN_TOP_MID, 0, L.pairing_release_y);" in ui


def test_generic_small_display_fallback_follows_exact_profiles() -> None:
    header = (ROOT / "firmware" / "src" / "ui_layout.h").read_text(encoding="utf-8")
    landscape = header.index("if (width == 320 && height == 240)")
    portrait = header.index("else if (width == 240 && height == 320)")
    fallback = header.index("else if (height <= 320)")
    assert landscape < portrait < fallback


def test_activity_footer_uses_activity_specific_freshness_state() -> None:
    ui = (ROOT / "firmware" / "src" / "ui.cpp").read_text(encoding="utf-8")
    normalized = " ".join(ui.split())
    assert 'lv_label_set_text(lbl_title, "Claude");' in ui
    assert "lv_label_set_text(lbl_title, tbuf);" not in ui
    assert "activity_freshness_apply(activity_freshness, updates, millis());" in normalized
    assert "format_activity_freshness(" in ui
    assert "activity_footer_label" in ui
    assert "L.footer_y" in ui


def test_activity_uses_independent_semantic_metric_widgets() -> None:
    ui = (ROOT / "firmware" / "src" / "ui.cpp").read_text(encoding="utf-8")

    for widget in (
        "lbl_activity_open",
        "lbl_activity_busy",
        "lbl_activity_waiting",
        "lbl_activity_unread",
    ):
        assert f"static lv_obj_t* {widget};" in ui
        assert f"{widget} = lv_label_create" in ui

    assert '"Open     %d\\nBusy     %d\\nWaiting  %d"' not in ui
    assert 'lv_label_set_text_fmt(lbl_activity_open, "Open  %d"' in ui
    assert 'lv_label_set_text_fmt(lbl_activity_busy, "Busy  %d"' in ui
    assert 'lv_label_set_text_fmt(lbl_activity_waiting, "Waiting  %d"' in ui
    assert 'lv_label_set_text_fmt(lbl_activity_unread, "Unread  %d"' in ui


def test_activity_rows_handle_layout_and_provider_availability() -> None:
    ui = (ROOT / "firmware" / "src" / "ui.cpp").read_text(encoding="utf-8")
    normalized = " ".join(ui.split())

    assert "const bool compact_activity = L.usage_panel_h <= 90;" in normalized
    assert "compact_activity ? 20 : 28" in normalized
    assert "compact_activity ? 40 : 52" in normalized
    assert "compact_activity ? 60 : 76" in normalized
    assert "compact_activity ? 28 : 36" in normalized
    assert "lbl_activity_claude_unavailable" in ui
    assert "lbl_activity_codex_unavailable" in ui
    assert "lv_obj_add_flag(lbl_activity_open, LV_OBJ_FLAG_HIDDEN);" in ui
    assert "lv_obj_clear_flag(lbl_activity_open, LV_OBJ_FLAG_HIDDEN);" in ui
    assert "lv_obj_add_flag(lbl_activity_unread, LV_OBJ_FLAG_HIDDEN);" in ui
    assert "lv_obj_clear_flag(lbl_activity_unread, LV_OBJ_FLAG_HIDDEN);" in ui
    assert "lv_obj_set_style_text_color(lbl_activity_claude_unavailable, COL_MUTED, 0);" in ui
    assert "lv_obj_set_style_text_color(lbl_activity_codex_unavailable, COL_MUTED, 0);" in ui


def test_display_ready_log_reports_selected_orientation_and_dimensions() -> None:
    display = (
        ROOT / "firmware/src/boards/esp32_2432s024c/display.cpp"
    ).read_text(encoding="utf-8")
    assert "LCD_WIDTH" in display
    assert "LCD_HEIGHT" in display
    assert "landscape" in display
    assert "portrait" in display
