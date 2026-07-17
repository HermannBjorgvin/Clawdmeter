# Dashboard UX and Bidirectional Navigation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the four-page one-way dashboard with three useful provider pages, provider-correct branding, semantic Activity colors, and left/right tap navigation.

**Architecture:** Keep daemon payloads and the board-local touch transform unchanged. Add pure carousel, tap-direction, branding-mask, and Activity-color helpers that Unity can exercise without a display; `ui.cpp` consumes those helpers to build and switch LVGL widgets. The mascot subsystem remains available to boot/pairing/idle code but is removed from the dashboard enum and carousel.

**Tech Stack:** C++17, PlatformIO, Arduino ESP32, LVGL 9.5, Unity, Python 3/pytest, Pillow for one-time RGB565A8 asset generation, official OpenAI Blossom source.

## Global Constraints

- Dashboard order is exactly Claude, Codex, Activity.
- Automatic navigation advances after 12 seconds; any manual direction defers it for 30 seconds.
- `x < screen_width / 2` means previous; `x >= screen_width / 2` means next.
- Claude Activity colors are Open `#55A868`, Busy `#D97757`, Waiting `#5B8FF9`; Codex Unread is `#8B7CF6`.
- The OpenAI Blossom remains monochrome, proportionally unmodified, and stored locally in firmware.
- Existing daemon payloads, Activity freshness semantics, board-local orientation, long-press, and wake-consumption behavior do not change.
- No push, merge, or production upload occurs during implementation tasks.

---

### Task 1: Bidirectional carousel primitives

**Files:**
- Modify: `firmware/src/dashboard_carousel.h`
- Modify: `firmware/test/test_port_helpers/test_main.cpp`

**Interfaces:**
- Consumes: existing `DashboardPage`, `CarouselState`, `DASHBOARD_MANUAL_HOLDOFF_MS`.
- Produces: `DashboardNavigationDirection`, `dashboard_previous_page(DashboardPage)`, `dashboard_direction_for_x(uint16_t, uint16_t)`, and `carousel_manual_previous(CarouselState&, uint32_t)`.

- [ ] **Step 1: Write failing Unity tests for reverse movement and tap halves**

Add beside the current carousel tests:

```cpp
void test_carousel_previous_moves_in_reverse_order(void) {
    CarouselState state{};
    carousel_start(state, DASHBOARD_ACTIVITY, 1000);
    TEST_ASSERT_EQUAL(DASHBOARD_CODEX, carousel_manual_previous(state, 2000));
    TEST_ASSERT_EQUAL(DASHBOARD_CLAUDE, carousel_manual_previous(state, 3000));
}

void test_touch_halves_select_previous_and_next(void) {
    TEST_ASSERT_EQUAL(DASHBOARD_NAV_PREVIOUS, dashboard_direction_for_x(0, 320));
    TEST_ASSERT_EQUAL(DASHBOARD_NAV_PREVIOUS, dashboard_direction_for_x(159, 320));
    TEST_ASSERT_EQUAL(DASHBOARD_NAV_NEXT, dashboard_direction_for_x(160, 320));
    TEST_ASSERT_EQUAL(DASHBOARD_NAV_NEXT, dashboard_direction_for_x(319, 320));
    TEST_ASSERT_EQUAL(DASHBOARD_NAV_PREVIOUS, dashboard_direction_for_x(119, 240));
    TEST_ASSERT_EQUAL(DASHBOARD_NAV_NEXT, dashboard_direction_for_x(120, 240));
}

void test_previous_touch_defers_auto_advance_for_thirty_seconds(void) {
    CarouselState state{};
    carousel_start(state, DASHBOARD_CODEX, 1000);
    carousel_manual_previous(state, 5000);
    TEST_ASSERT_FALSE(carousel_tick(state, 34999));
    TEST_ASSERT_TRUE(carousel_tick(state, 35000));
}
```

Register all three with `RUN_TEST`.

- [ ] **Step 2: Run Unity compile/link and verify RED**

Run:

```powershell
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' test -d firmware -e esp32_2432s024c -f test_port_helpers --without-uploading --without-testing
```

Expected: compilation fails because `carousel_manual_previous`, `dashboard_direction_for_x`, and direction constants do not exist.

- [ ] **Step 3: Implement minimal pure navigation helpers**

Add to `dashboard_carousel.h`:

```cpp
enum DashboardNavigationDirection : uint8_t {
    DASHBOARD_NAV_PREVIOUS,
    DASHBOARD_NAV_NEXT,
};

inline DashboardPage dashboard_previous_page(DashboardPage page) {
    const uint8_t value = static_cast<uint8_t>(page);
    return static_cast<DashboardPage>(
        value == 0 ? DASHBOARD_PAGE_COUNT - 1 : value - 1
    );
}

inline DashboardNavigationDirection dashboard_direction_for_x(
    uint16_t x,
    uint16_t screen_width
) {
    return x < screen_width / 2
        ? DASHBOARD_NAV_PREVIOUS
        : DASHBOARD_NAV_NEXT;
}

inline DashboardPage carousel_manual_previous(CarouselState& state, uint32_t now) {
    state.page = dashboard_previous_page(state.page);
    state.next_advance_ms = now + DASHBOARD_MANUAL_HOLDOFF_MS;
    state.started = true;
    return state.page;
}
```

- [ ] **Step 4: Run focused Unity compile/link and full Python suite**

Run:

```powershell
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' test -d firmware -e esp32_2432s024c -f test_port_helpers --without-uploading --without-testing
& 'C:\Users\Gustavo\Documents\esp32-claude\.venv\Scripts\python.exe' -m pytest daemon\tests tests -q
```

Expected: Unity target passes; Python suite passes with only the two existing coroutine warnings.

- [ ] **Step 5: Commit navigation primitives**

```powershell
git add firmware/src/dashboard_carousel.h firmware/test/test_port_helpers/test_main.cpp
git commit -m "feat: add bidirectional dashboard navigation"
```

---

### Task 2: Provider branding and official Codex mark

**Files:**
- Create: `assets/openai_blossom_80.png`
- Create: `firmware/src/codex_logo.h`
- Create: `firmware/src/dashboard_branding.h`
- Modify: `firmware/src/ui.cpp`
- Modify: `firmware/test/test_port_helpers/test_main.cpp`
- Modify: `tests/test_esp32_2432s024c_contract.py`

**Interfaces:**
- Consumes: `DashboardPage`, existing Claude `logo_data`, `L.logo_scale`, and `L.logo_rendered_width`.
- Produces: `CODEX_LOGO_WIDTH`, `CODEX_LOGO_HEIGHT`, `codex_logo_data`, `DashboardBrandMask`, and `dashboard_brand_mask(DashboardPage)`.

- [ ] **Step 1: Write failing branding-mask and asset contract tests**

Add a pure helper test:

```cpp
void test_provider_branding_matches_each_dashboard_page(void) {
    TEST_ASSERT_EQUAL(DASHBOARD_BRAND_CLAUDE, dashboard_brand_mask(DASHBOARD_CLAUDE));
    TEST_ASSERT_EQUAL(DASHBOARD_BRAND_CODEX, dashboard_brand_mask(DASHBOARD_CODEX));
    TEST_ASSERT_EQUAL(
        DASHBOARD_BRAND_CLAUDE | DASHBOARD_BRAND_CODEX,
        dashboard_brand_mask(DASHBOARD_ACTIVITY)
    );
}
```

Include `dashboard_branding.h` and register the test. Add Python contracts asserting that `codex_logo.h` declares an 80x80 RGB565A8 array of 19,200 bytes and that `ui.cpp` owns separate `claude_logo_img` and `codex_logo_img` objects.

- [ ] **Step 2: Run focused tests and verify RED**

Run:

```powershell
& 'C:\Users\Gustavo\Documents\esp32-claude\.venv\Scripts\python.exe' -m pytest tests\test_esp32_2432s024c_contract.py -q
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' test -d firmware -e esp32_2432s024c -f test_port_helpers --without-uploading --without-testing
```

Expected: missing branding header, Codex asset, and UI image objects fail the contracts.

- [ ] **Step 3: Create the local official monochrome asset**

Use the OpenAI Blossom from `https://openai.com/brand/`, retain its proportions, render white on transparency at 80x80 with prescribed clear space, and save `assets/openai_blossom_80.png`. Convert it mechanically to LVGL RGB565A8 using Pillow:

```python
from pathlib import Path
from PIL import Image

source = Image.open("assets/openai_blossom_80.png").convert("RGBA")
assert source.size == (80, 80)
rgb565 = bytearray()
alpha = bytearray()
for r, g, b, a in source.getdata():
    value = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    rgb565.extend((value & 0xFF, value >> 8))
    alpha.append(a)
data = rgb565 + alpha
assert len(data) == 19200
rows = [", ".join(f"0x{byte:02X}" for byte in data[i:i + 16]) for i in range(0, len(data), 16)]
Path("firmware/src/codex_logo.h").write_text(
    "#pragma once\n#include <stdint.h>\n\n"
    "#define CODEX_LOGO_WIDTH 80\n#define CODEX_LOGO_HEIGHT 80\n"
    "static const uint8_t codex_logo_data[19200] = {\n    "
    + ",\n    ".join(rows)
    + "\n};\n",
    encoding="utf-8",
)
```

- [ ] **Step 4: Implement the branding mask**

Create `dashboard_branding.h`:

```cpp
#pragma once

#include <stdint.h>
#include "dashboard_carousel.h"

enum DashboardBrandMask : uint8_t {
    DASHBOARD_BRAND_NONE = 0,
    DASHBOARD_BRAND_CLAUDE = 1,
    DASHBOARD_BRAND_CODEX = 2,
};

inline uint8_t dashboard_brand_mask(DashboardPage page) {
    if (page == DASHBOARD_CLAUDE) return DASHBOARD_BRAND_CLAUDE;
    if (page == DASHBOARD_CODEX) return DASHBOARD_BRAND_CODEX;
    if (page == DASHBOARD_ACTIVITY) {
        return DASHBOARD_BRAND_CLAUDE | DASHBOARD_BRAND_CODEX;
    }
    return DASHBOARD_BRAND_NONE;
}
```

- [ ] **Step 5: Create two page-aware LVGL header images**

In `ui.cpp`, include both new headers; replace `logo_img`/`logo_dsc` with Claude and Codex pairs. Initialize both descriptors as RGB565A8, give both top-left pivots and the same scale, place Claude at `L.margin`, and place Codex at `L.margin` except on Activity, where it moves to `L.scr_w - L.margin - L.logo_rendered_width`.

Add:

```cpp
static void apply_brand_visibility(DashboardPage page) {
    const uint8_t mask = dashboard_brand_mask(page);
    const bool activity = page == DASHBOARD_ACTIVITY;
    lv_obj_set_pos(
        codex_logo_img,
        activity ? L.scr_w - L.margin - L.logo_rendered_width : L.margin,
        (L.horizontal_cards || L.logo_size == 48) ? 6 : L.title_y - 10
    );
    if (mask & DASHBOARD_BRAND_CLAUDE) lv_obj_clear_flag(claude_logo_img, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(claude_logo_img, LV_OBJ_FLAG_HIDDEN);
    if (mask & DASHBOARD_BRAND_CODEX) lv_obj_clear_flag(codex_logo_img, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(codex_logo_img, LV_OBJ_FLAG_HIDDEN);
}
```

Call it from `ui_show_screen(page)` and move both images to the foreground before the transparent navigation layer.

- [ ] **Step 6: Run branding contracts, Unity compile/link, and both builds**

Run:

```powershell
& 'C:\Users\Gustavo\Documents\esp32-claude\.venv\Scripts\python.exe' -m pytest tests\test_esp32_2432s024c_contract.py -q
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' test -d firmware -e esp32_2432s024c -f test_port_helpers --without-uploading --without-testing
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' run -d firmware -e esp32_2432s024c -j 1
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' run -d firmware -e esp32_2432s024c_landscape -j 1
```

Expected: all commands exit 0; logo bounds remain inside 240x320 and 320x240.

- [ ] **Step 7: Commit provider branding**

```powershell
git add assets/openai_blossom_80.png firmware/src/codex_logo.h firmware/src/dashboard_branding.h firmware/src/ui.cpp firmware/test/test_port_helpers/test_main.cpp tests/test_esp32_2432s024c_contract.py
git commit -m "feat: add page-aware Claude and Codex branding"
```

---

### Task 3: Semantic Activity rows

**Files:**
- Create: `firmware/src/activity_style.h`
- Modify: `firmware/src/ui.cpp`
- Modify: `firmware/test/test_port_helpers/test_main.cpp`
- Modify: `tests/test_esp32_2432s024c_contract.py`

**Interfaces:**
- Consumes: `ActivityData`, existing Activity panels, `L.detail_font`, and Activity freshness state.
- Produces: `ACTIVITY_OPEN_HEX`, `ACTIVITY_BUSY_HEX`, `ACTIVITY_WAITING_HEX`, `ACTIVITY_UNREAD_HEX` and individual row labels.

- [ ] **Step 1: Write failing color and widget contracts**

Add Unity assertions:

```cpp
void test_activity_status_colors_are_semantically_distinct(void) {
    TEST_ASSERT_EQUAL_HEX32(0x55A868, ACTIVITY_OPEN_HEX);
    TEST_ASSERT_EQUAL_HEX32(0xD97757, ACTIVITY_BUSY_HEX);
    TEST_ASSERT_EQUAL_HEX32(0x5B8FF9, ACTIVITY_WAITING_HEX);
    TEST_ASSERT_EQUAL_HEX32(0x8B7CF6, ACTIVITY_UNREAD_HEX);
    TEST_ASSERT_NOT_EQUAL(ACTIVITY_OPEN_HEX, ACTIVITY_BUSY_HEX);
    TEST_ASSERT_NOT_EQUAL(ACTIVITY_BUSY_HEX, ACTIVITY_WAITING_HEX);
}
```

Add Python contracts requiring separate `lbl_activity_open`, `lbl_activity_busy`, `lbl_activity_waiting`, and `lbl_activity_unread` widgets; forbid the old combined format string `"Open     %d\nBusy     %d\nWaiting  %d"`.

- [ ] **Step 2: Run tests and verify RED**

Run the same focused Python and Unity compile/link commands from Task 2. Expected: missing constants and row objects fail.

- [ ] **Step 3: Add semantic color constants**

Create `activity_style.h`:

```cpp
#pragma once
#include <stdint.h>

constexpr uint32_t ACTIVITY_OPEN_HEX = 0x55A868;
constexpr uint32_t ACTIVITY_BUSY_HEX = 0xD97757;
constexpr uint32_t ACTIVITY_WAITING_HEX = 0x5B8FF9;
constexpr uint32_t ACTIVITY_UNREAD_HEX = 0x8B7CF6;
```

- [ ] **Step 4: Replace combined Activity labels with provider title and row labels**

Keep `Claude Code` and `Codex` title labels in normal foreground color. Create one label per metric, position them vertically within the existing panel, and apply `lv_color_hex` using the constants. Use `const bool compact_activity = L.usage_panel_h <= 90;`: compact 240x320 panels use Claude row Y positions 20, 40, and 60 and Codex Unread Y 28; taller 320x240 and large panels use 28, 52, and 76 and Codex Unread Y 36. These bounds keep the last 14-pixel line within the 88-pixel portrait panel.

In `ui_update`, set independent strings:

```cpp
lv_label_set_text_fmt(lbl_activity_open, "Open  %d", data->activity.claude_open);
lv_label_set_text_fmt(lbl_activity_busy, "Busy  %d", data->activity.claude_busy);
lv_label_set_text_fmt(lbl_activity_waiting, "Waiting  %d", data->activity.claude_waiting);
lv_label_set_text_fmt(lbl_activity_unread, "Unread  %d", data->activity.codex_unread);
```

When a provider is unavailable, hide its metric rows and show `Unavailable` in muted color under the provider title. When it becomes valid again, hide the unavailable label and restore the relevant rows. Do not convert unavailable values to zero. Leave `activity_footer_label` and freshness updates untouched.

- [ ] **Step 5: Run focused tests, full suite, and both builds**

Run:

```powershell
& 'C:\Users\Gustavo\Documents\esp32-claude\.venv\Scripts\python.exe' -m pytest tests\test_esp32_2432s024c_contract.py -q
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' test -d firmware -e esp32_2432s024c -f test_port_helpers --without-uploading --without-testing
& 'C:\Users\Gustavo\Documents\esp32-claude\.venv\Scripts\python.exe' -m pytest daemon\tests tests -q
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' run -d firmware -e esp32_2432s024c -j 1
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' run -d firmware -e esp32_2432s024c_landscape -j 1
```

Expected: all tests and builds pass; only the two known Python coroutine warnings remain.

- [ ] **Step 6: Commit semantic Activity rows**

```powershell
git add firmware/src/activity_style.h firmware/src/ui.cpp firmware/test/test_port_helpers/test_main.cpp tests/test_esp32_2432s024c_contract.py
git commit -m "feat: color Activity states by meaning"
```

---

### Task 4: Three-page UI integration and left/right taps

**Files:**
- Modify: `firmware/src/dashboard_carousel.h`
- Modify: `firmware/src/ui.cpp`
- Modify: `firmware/test/test_port_helpers/test_main.cpp`
- Modify: `tests/test_esp32_2432s024c_contract.py`

**Interfaces:**
- Consumes: Task 1 navigation helpers, Task 2 branding mask, existing LVGL `navigation_layer`.
- Produces: a three-page dashboard with coordinate-routed manual navigation and no Robot dashboard page.

- [ ] **Step 1: Write failing tests for exact three-page order and no Robot page**

Replace the old approved-order test with:

```cpp
void test_carousel_wraps_across_exactly_three_pages(void) {
    TEST_ASSERT_EQUAL_INT(3, DASHBOARD_PAGE_COUNT);
    CarouselState state{};
    carousel_start(state, DASHBOARD_CLAUDE, 1000);
    TEST_ASSERT_EQUAL(DASHBOARD_CODEX, carousel_manual_next(state, 2000));
    TEST_ASSERT_EQUAL(DASHBOARD_ACTIVITY, carousel_manual_next(state, 3000));
    TEST_ASSERT_EQUAL(DASHBOARD_CLAUDE, carousel_manual_next(state, 4000));
}

void test_three_page_carousel_wraps_in_reverse_order(void) {
    CarouselState state{};
    carousel_start(state, DASHBOARD_CLAUDE, 1000);
    TEST_ASSERT_EQUAL(DASHBOARD_ACTIVITY, carousel_manual_previous(state, 2000));
    TEST_ASSERT_EQUAL(DASHBOARD_CODEX, carousel_manual_previous(state, 3000));
    TEST_ASSERT_EQUAL(DASHBOARD_CLAUDE, carousel_manual_previous(state, 4000));
}
```

Add Python contracts asserting `DASHBOARD_ROBOT` is absent from `dashboard_carousel.h`, `robot_status_label` and transport freshness copy are absent from `ui.cpp`, and the click callback calls both `carousel_manual_previous` and `carousel_manual_next` after reading `lv_indev_get_point`.

- [ ] **Step 2: Run focused tests and verify RED**

Run focused Python and Unity compile/link commands. Expected: page count remains four and Robot symbols remain present.

- [ ] **Step 3: Remove Robot from the dashboard enum and UI visibility flow**

Change the enum to:

```cpp
enum DashboardPage : uint8_t {
    DASHBOARD_CLAUDE,
    DASHBOARD_CODEX,
    DASHBOARD_ACTIVITY,
    DASHBOARD_PAGE_COUNT,
};
```

Remove `robot_status_label`, `last_received_update_ms`, `last_robot_status_refresh_ms`, and `last_received_transport` from `ui.cpp`; remove Robot-specific creation, tick updates, switch case, indicator hiding, battery special case, and logo special case. Do not delete `splash.cpp`, splash assets, or boot/pairing/idle calls outside dashboard page switching.

- [ ] **Step 4: Route short taps by logical X coordinate**

Replace `global_click_cb` with:

```cpp
static void global_click_cb(lv_event_t* e) {
    (void)e;
    lv_indev_t* indev = lv_indev_active();
    if (!indev) return;

    lv_point_t point{};
    lv_indev_get_point(indev, &point);
    const uint32_t now = millis();
    if (!carousel.started) carousel_start(carousel, current_page, now);

    const DashboardNavigationDirection direction = dashboard_direction_for_x(
        static_cast<uint16_t>(point.x < 0 ? 0 : point.x),
        static_cast<uint16_t>(L.scr_w)
    );
    const DashboardPage page = direction == DASHBOARD_NAV_PREVIOUS
        ? carousel_manual_previous(carousel, now)
        : carousel_manual_next(carousel, now);
    ui_show_screen(page);
}
```

Keep the navigation layer full-screen and in the foreground. Do not add visible arrows or gesture recognition.

- [ ] **Step 5: Run focused tests, full suite, and both builds**

Run all commands from Task 3 Step 5. Expected: three-page tests pass, `ui.cpp` compiles with no Robot dashboard references, and both builds succeed.

- [ ] **Step 6: Commit the integrated three-page UI**

```powershell
git add firmware/src/dashboard_carousel.h firmware/src/ui.cpp firmware/test/test_port_helpers/test_main.cpp tests/test_esp32_2432s024c_contract.py
git commit -m "feat: navigate three dashboard pages in both directions"
```

---

### Task 5: Independent review, physical verification, and production restore

**Files:**
- Modify only if review finds an in-scope defect: files already listed in Tasks 1-4.
- Record evidence: `.superpowers/sdd/dashboard-ux-final-report.md` (ignored workflow artifact).

**Interfaces:**
- Consumes: completed three-page firmware and connected ESP32-2432S024C on COM3.
- Produces: reviewed, hash-verified landscape firmware running with active serial updates.

- [ ] **Step 1: Run independent static review**

Review the complete feature range for Critical, Important, and Minor findings. Require zero Critical and Important issues before hardware access. Review exact page count, logo visibility, Activity colors, unavailable states, touch boundary, automatic timing, portrait safety, and absence of payload changes.

- [ ] **Step 2: Run fresh software verification**

Run:

```powershell
& 'C:\Users\Gustavo\Documents\esp32-claude\.venv\Scripts\python.exe' -m pytest daemon\tests tests -q
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' test -d firmware -e esp32_2432s024c -f test_port_helpers --without-uploading --without-testing
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' run -d firmware -e esp32_2432s024c -j 1
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' run -d firmware -e esp32_2432s024c_landscape -j 1
```

Expected: all commands exit 0; document exact test counts and RAM/flash usage.

- [ ] **Step 3: Stop only the verified tray process tree and run physical Unity**

Confirm each target PID command line contains `pythonw.exe -m daemon.tray_windows`, stop only that tree, then run:

```powershell
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' test -d firmware -e esp32_2432s024c -f test_port_helpers --upload-port COM3
```

Expected: every Unity case passes on the physical ESP32.

- [ ] **Step 4: Rebuild and flash the definitive landscape firmware**

Rebuild landscape after Unity, then use the proven conservative writer:

```powershell
$env:PYTHONUTF8='1'
$env:PYTHONIOENCODING='utf-8'
& 'C:\Users\Gustavo\.platformio\penv\Scripts\esptool.exe' `
  --chip esp32 --port COM3 --baud 115200 `
  --before default-reset --after hard-reset --no-stub `
  write-flash --no-compress --flash-mode dio --flash-freq 40m --flash-size detect `
  0x1000 firmware\.pio\build\esp32_2432s024c_landscape\bootloader.bin `
  0x8000 firmware\.pio\build\esp32_2432s024c_landscape\partitions.bin `
  0xe000 C:\Users\Gustavo\.platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin `
  0x10000 firmware\.pio\build\esp32_2432s024c_landscape\firmware.bin
```

Expected: four `Hash of data verified` lines and `Hard resetting via RTS pin`.

- [ ] **Step 5: Restart the hidden tray and verify serial ACKs**

Start `C:\Users\Gustavo\Documents\esp32-claude\.venv\Scripts\pythonw.exe -m daemon.tray_windows` hidden from the feature worktree. Require `Clawdmeter identified on COM3`, at least two `Sending by USB serial` cycles separated by the 60-second poll, zero `acknowledgement failed`, and both tray PIDs alive.

- [ ] **Step 6: Record physical acceptance and final state**

Record build hashes, test counts, flash evidence, tray PIDs, and the visual checklist: exactly three pages, no Robot/USB status page, Claude mark on Claude, Blossom on Codex, both marks on Activity, semantic colors, left tap previous, right tap next, upright and unmirrored rendering. Keep the branch/worktree intact and do not push.
