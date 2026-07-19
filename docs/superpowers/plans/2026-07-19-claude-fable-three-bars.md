# Claude Fable Three-Bar Dashboard Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Display the real Claude Fable weekly scoped allowance as a third compact usage row on the ESP32-2432S024C 320x240 Claude page and move the animated status word beside the page dots.

**Architecture:** Add a focused Windows host collector for the OAuth usage endpoint, merge its optional compact fields into the existing Claude payload, and extend the firmware parser with backward-compatible Fable state. The 320x240 Claude page gets a dedicated three-row layout; Codex, Activity, carousel navigation, and all other display layouts keep their existing geometry.

**Tech Stack:** Python 3.13, `httpx`, `pytest`, Arduino/C++17, ArduinoJson 7, LVGL 9, PlatformIO, Unity.

## Global Constraints

- Use `GET https://api.anthropic.com/api/oauth/usage` with the existing OAuth token and `anthropic-beta: oauth-2025-04-20`.
- Select only an active `weekly_scoped` item whose model `display_name` is exactly `Fable`.
- Cache a successful Fable response for 180 seconds.
- Never fabricate `0%`; absence of `f` means unavailable, while `f: 0` is a valid measurement.
- Add optional compact payload keys `f` and `fr`; preserve compatibility with old host and firmware versions.
- Apply the three-row visual layout only to the 320x240 landscape profile.
- Use rows at y=52, y=104, and y=156, each 47 pixels high with 5-pixel gaps.
- Use 8-pixel progress bars and existing green/amber/red percentage thresholds.
- Keep exactly three page dots and place a 14-pixel status word beside them without a glyph, ellipsis, separator, or overlap.
- Do not change Codex, Activity, carousel order, automatic timing, or left/right touch navigation.

---

## File Structure

- Create `daemon/fable_usage.py`: parse, fetch, and cache the optional Fable scoped allowance.
- Create `daemon/tests/test_fable_usage.py`: pure parser and cache/network behavior tests.
- Modify `daemon/claude_usage_daemon_serial_windows.py`: merge Fable fields into a successful Claude payload before building the dashboard payload.
- Modify `daemon/tests/test_windows_serial.py`: verify independent Fable integration and wire-size safety.
- Modify `firmware/src/data.h`: store optional Fable percentage, reset minutes, and validity.
- Modify `firmware/src/dashboard_payload.cpp`: parse optional `f` and `fr` fields.
- Modify `firmware/src/ui_layout.h`: define the 320x240 compact-row and compact-footer geometry.
- Modify `firmware/src/ui.cpp`: build/update the Fable row and compact footer presentation.
- Modify `firmware/test/test_port_helpers/test_main.cpp`: Unity coverage for parsing and geometry.
- Modify `tests/test_esp32_2432s024c_contract.py`: source contracts for the selected layout and footer.
- Modify `README.md` and `docs/README.pt-BR.md`: document the Fable row and internal endpoint caveat.

---

### Task 1: Parse and cache Fable OAuth usage

**Files:**
- Create: `daemon/fable_usage.py`
- Create: `daemon/tests/test_fable_usage.py`

**Interfaces:**
- Consumes: existing Claude OAuth access token string.
- Produces: `extract_fable_usage(value: object, now: datetime | None = None) -> dict[str, int]` and `poll_fable_usage(token: str) -> dict[str, int]`.
- Output shape: `{}` when unavailable, `{"f": percent}` when reset is unknown, or `{"f": percent, "fr": minutes}` when reset is known.

- [ ] **Step 1: Write failing parser tests**

Create `daemon/tests/test_fable_usage.py` with fixed UTC timestamps so zero, reset conversion, malformed input, and exact model matching are deterministic:

```python
from __future__ import annotations

import asyncio
from datetime import datetime, timezone
from unittest.mock import AsyncMock, MagicMock

from daemon import fable_usage


NOW = datetime(2026, 7, 19, 3, 0, tzinfo=timezone.utc)


def fable_limit(percent: int = 61) -> dict:
    return {
        "kind": "weekly_scoped",
        "group": "weekly",
        "percent": percent,
        "severity": "normal",
        "resets_at": "2026-07-19T05:00:00+00:00",
        "scope": {"model": {"id": None, "display_name": "Fable"}},
        "is_active": True,
    }


def test_extract_fable_usage_reads_percent_and_reset_minutes() -> None:
    assert fable_usage.extract_fable_usage(
        {"limits": [fable_limit()]}, now=NOW
    ) == {"f": 61, "fr": 120}


def test_extract_fable_usage_preserves_measured_zero() -> None:
    assert fable_usage.extract_fable_usage(
        {"limits": [fable_limit(0)]}, now=NOW
    ) == {"f": 0, "fr": 120}


def test_extract_fable_usage_rejects_inactive_wrong_or_malformed_limits() -> None:
    inactive = fable_limit()
    inactive["is_active"] = False
    wrong = fable_limit()
    wrong["scope"]["model"]["display_name"] = "Opus"
    malformed = fable_limit()
    malformed["percent"] = "61"

    assert fable_usage.extract_fable_usage(
        {"limits": [inactive, wrong, malformed]}, now=NOW
    ) == {}


def test_extract_fable_usage_keeps_percent_when_reset_is_missing() -> None:
    limit = fable_limit()
    del limit["resets_at"]

    assert fable_usage.extract_fable_usage({"limits": [limit]}, now=NOW) == {"f": 61}
```

- [ ] **Step 2: Run the parser tests and verify they fail**

Run:

```powershell
& 'C:\Users\Gustavo\Documents\esp32-claude\.venv\Scripts\python.exe' -m pytest daemon/tests/test_fable_usage.py -q
```

Expected: collection fails because `daemon.fable_usage` does not exist.

- [ ] **Step 3: Implement the pure parser and cached poller**

Create `daemon/fable_usage.py`:

```python
from __future__ import annotations

import math
import time
from datetime import datetime, timezone
from numbers import Real
from typing import Any

import httpx


FABLE_USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
FABLE_CACHE_SECONDS = 180.0

_cached_usage: dict[str, int] | None = None
_cached_at = 0.0


def _reset_cache() -> None:
    global _cached_usage, _cached_at
    _cached_usage = None
    _cached_at = 0.0


def extract_fable_usage(
    value: object,
    now: datetime | None = None,
) -> dict[str, int]:
    if not isinstance(value, dict) or not isinstance(value.get("limits"), list):
        return {}

    current = now or datetime.now(timezone.utc)
    for limit in value["limits"]:
        if not isinstance(limit, dict):
            continue
        scope = limit.get("scope")
        model = scope.get("model") if isinstance(scope, dict) else None
        percent = limit.get("percent")
        if (
            limit.get("kind") != "weekly_scoped"
            or limit.get("is_active") is not True
            or not isinstance(model, dict)
            or model.get("display_name") != "Fable"
            or isinstance(percent, bool)
            or not isinstance(percent, Real)
        ):
            continue

        result = {"f": max(0, min(100, int(round(float(percent)))))}
        resets_at = limit.get("resets_at")
        if isinstance(resets_at, str):
            try:
                reset = datetime.fromisoformat(resets_at.replace("Z", "+00:00"))
                if reset.tzinfo is None:
                    reset = reset.replace(tzinfo=timezone.utc)
                result["fr"] = max(
                    0,
                    math.ceil((reset.astimezone(timezone.utc) - current).total_seconds() / 60),
                )
            except ValueError:
                pass
        return result
    return {}


async def poll_fable_usage(token: str) -> dict[str, int]:
    global _cached_usage, _cached_at

    monotonic_now = time.monotonic()
    if _cached_usage is not None and monotonic_now - _cached_at < FABLE_CACHE_SECONDS:
        return dict(_cached_usage)

    headers = {
        "Authorization": f"Bearer {token}",
        "anthropic-beta": "oauth-2025-04-20",
    }
    try:
        async with httpx.AsyncClient(timeout=5.0) as client:
            response = await client.get(FABLE_USAGE_URL, headers=headers)
        if response.status_code != 200:
            return {}
        parsed = extract_fable_usage(response.json())
    except (httpx.HTTPError, ValueError):
        return {}
    if not parsed:
        return {}
    _cached_usage = parsed
    _cached_at = monotonic_now
    return dict(parsed)
```

- [ ] **Step 4: Add cache and failure tests**

Append tests using an async context-manager mock. Reset module state before each test and prove that a cached successful result avoids a second GET while HTTP failure returns `{}`:

```python
def test_poll_fable_usage_caches_success_for_180_seconds(monkeypatch) -> None:
    response = MagicMock()
    response.status_code = 200
    response.json.return_value = {"limits": [fable_limit()]}
    client = AsyncMock()
    client.__aenter__.return_value = client
    client.__aexit__.return_value = False
    client.get.return_value = response

    fable_usage._reset_cache()
    monkeypatch.setattr(fable_usage.httpx, "AsyncClient", lambda **_kwargs: client)
    monkeypatch.setattr(fable_usage.time, "monotonic", lambda: 100.0)

    first = asyncio.run(fable_usage.poll_fable_usage("token"))
    second = asyncio.run(fable_usage.poll_fable_usage("token"))

    assert first["f"] == 61
    assert second == first
    client.get.assert_awaited_once()


def test_poll_fable_usage_failure_returns_unavailable(monkeypatch) -> None:
    client = AsyncMock()
    client.__aenter__.return_value = client
    client.__aexit__.return_value = False
    client.get.side_effect = httpx.ConnectError("offline")

    fable_usage._reset_cache()
    monkeypatch.setattr(fable_usage.httpx, "AsyncClient", lambda **_kwargs: client)

    assert asyncio.run(fable_usage.poll_fable_usage("token")) == {}
```

Add `import httpx` to the test file for the failure test.

- [ ] **Step 5: Run tests and commit**

Run:

```powershell
& 'C:\Users\Gustavo\Documents\esp32-claude\.venv\Scripts\python.exe' -m pytest daemon/tests/test_fable_usage.py -q
```

Expected: all Fable collector tests pass.

Commit:

```powershell
git add daemon/fable_usage.py daemon/tests/test_fable_usage.py
git commit -m "feat: collect Claude Fable usage"
```

---

### Task 2: Merge Fable into the Windows serial payload

**Files:**
- Modify: `daemon/claude_usage_daemon_serial_windows.py`
- Modify: `daemon/tests/test_windows_serial.py`

**Interfaces:**
- Consumes: `poll_fable_usage(token: str) -> dict[str, int]` from Task 1.
- Produces: the existing `build_dashboard_payload()` input enriched with optional `f` and `fr` keys.

- [ ] **Step 1: Write the failing integration test**

Add a test that runs one daemon iteration and captures the merged payload:

```python
def test_connect_and_run_merges_fable_into_successful_claude_payload(monkeypatch):
    stop_event = asyncio.Event()
    session = SimpleNamespace(write_payload=MagicMock(return_value=True), close=MagicMock())

    async def stop_after_first_iteration(_event, timeout):
        stop_event.set()

    async def fake_poll_api(_token):
        return {"s": 56, "sr": 120, "w": 33, "wr": 1440, "ok": True}

    async def fake_poll_fable(_token):
        return {"f": 61, "fr": 120}

    monkeypatch.setattr(serial_daemon, "read_token", lambda: "token")
    monkeypatch.setattr(serial_daemon, "poll_api", fake_poll_api)
    monkeypatch.setattr(serial_daemon, "poll_fable_usage", fake_poll_fable, raising=False)
    monkeypatch.setattr(
        serial_daemon,
        "build_dashboard_payload",
        lambda claude_payload, _home: claude_payload,
    )
    monkeypatch.setattr(serial_daemon, "_wait_first", stop_after_first_iteration)

    assert asyncio.run(serial_daemon.connect_and_run(session, stop_event)) is True
    session.write_payload.assert_called_once_with(
        {"s": 56, "sr": 120, "w": 33, "wr": 1440, "ok": True, "f": 61, "fr": 120}
    )
```

- [ ] **Step 2: Run the integration test and verify it fails**

Run:

```powershell
& 'C:\Users\Gustavo\Documents\esp32-claude\.venv\Scripts\python.exe' -m pytest daemon/tests/test_windows_serial.py::test_connect_and_run_merges_fable_into_successful_claude_payload -q
```

Expected: FAIL because `poll_fable_usage` is not imported or invoked.

- [ ] **Step 3: Merge Fable without coupling failure to the regular Claude poll**

Import the collector:

```python
from daemon.fable_usage import poll_fable_usage
```

Immediately after a successful `poll_api(token)` call in `connect_and_run`, merge only the optional fields:

```python
claude_payload = await poll_api(token)
if claude_payload is not None:
    claude_payload.update(await poll_fable_usage(token))
```

Do not call the Fable endpoint when no Claude token exists. Do not change the existing `AuthError`, local-only payload, serial acknowledgement, or reconnect paths.

- [ ] **Step 4: Extend the maximum payload fixture**

Add these keys to `test_extended_dashboard_payload_fits_firmware_command_buffer`:

```python
"f": 100,
"fr": 10080,
```

Keep the existing `< 768` assertion.

- [ ] **Step 5: Run the serial tests and commit**

Run:

```powershell
& 'C:\Users\Gustavo\Documents\esp32-claude\.venv\Scripts\python.exe' -m pytest daemon/tests/test_windows_serial.py daemon/tests/test_fable_usage.py -q
```

Expected: all tests pass.

Commit:

```powershell
git add daemon/claude_usage_daemon_serial_windows.py daemon/tests/test_windows_serial.py
git commit -m "feat: send Fable usage over USB"
```

---

### Task 3: Parse optional Fable fields in firmware

**Files:**
- Modify: `firmware/src/data.h`
- Modify: `firmware/src/dashboard_payload.cpp`
- Modify: `firmware/test/test_port_helpers/test_main.cpp`

**Interfaces:**
- Consumes: optional top-level JSON keys `f` and `fr`.
- Produces: `UsageData::fable_valid`, `UsageData::fable_pct`, and `UsageData::fable_reset_mins`.

- [ ] **Step 1: Write failing Unity parser tests**

Add these tests after `test_old_claude_payload_remains_compatible`:

```cpp
void test_fable_payload_preserves_valid_zero_and_reset(void) {
    UsageData data{};
    uint8_t mask = parse_dashboard_json(
        "{\"s\":10,\"w\":20,\"f\":0,\"fr\":120,\"ok\":true}",
        &data
    );

    TEST_ASSERT_BITS_HIGH(DASHBOARD_UPDATE_CLAUDE, mask);
    TEST_ASSERT_TRUE(data.fable_valid);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, data.fable_pct);
    TEST_ASSERT_EQUAL_INT(120, data.fable_reset_mins);
}


void test_old_claude_payload_marks_fable_unavailable(void) {
    UsageData data{};
    parse_dashboard_json("{\"s\":10,\"w\":20,\"ok\":true}", &data);

    TEST_ASSERT_FALSE(data.fable_valid);
    TEST_ASSERT_EQUAL_INT(-1, data.fable_reset_mins);
}


void test_fable_percentage_is_clamped_to_display_range(void) {
    UsageData data{};
    parse_dashboard_json("{\"s\":10,\"f\":140,\"ok\":true}", &data);

    TEST_ASSERT_TRUE(data.fable_valid);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, data.fable_pct);
}


void test_malformed_fable_percentage_is_unavailable(void) {
    UsageData data{};
    parse_dashboard_json("{\"s\":10,\"f\":\"bad\",\"ok\":true}", &data);

    TEST_ASSERT_FALSE(data.fable_valid);
}
```

Register all four tests in `setup()`.

- [ ] **Step 2: Compile the test firmware and verify it fails**

Run:

```powershell
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' test -d firmware -e esp32_2432s024c -f test_port_helpers --without-uploading --without-testing
```

Expected: compile fails because the Fable members do not exist.

- [ ] **Step 3: Add Fable state to `UsageData`**

Insert after the weekly fields in `firmware/src/data.h`:

```cpp
float fable_pct;          // Fable weekly scoped utilization 0-100
int fable_reset_mins;     // minutes until scoped reset; -1 when unknown
bool fable_valid;         // true when the payload explicitly contains f
```

- [ ] **Step 4: Parse and clamp the optional fields**

Inside the existing Claude block in `parse_dashboard_json`, set availability on every new Claude payload:

```cpp
out->fable_valid = false;
out->fable_reset_mins = -1;
JsonVariantConst fable = doc["f"];
if (!fable.isNull() && fable.is<float>()) {
    float value = fable.as<float>();
    if (value < 0.0f) value = 0.0f;
    if (value > 100.0f) value = 100.0f;
    out->fable_pct = value;
    out->fable_reset_mins = doc["fr"] | -1;
    out->fable_valid = true;
}
```

This must remain inside `if (!doc["s"].isNull())` so a local Codex/Activity-only payload does not clear the last Claude view.

- [ ] **Step 5: Compile the Unity tests and commit**

Run the Task 3 compile command again. Expected: successful test firmware build.

Commit:

```powershell
git add firmware/src/data.h firmware/src/dashboard_payload.cpp firmware/test/test_port_helpers/test_main.cpp
git commit -m "feat: parse Fable usage on device"
```

---

### Task 4: Render three compact rows and compact the footer

**Files:**
- Modify: `firmware/src/ui_layout.h`
- Modify: `firmware/src/ui.cpp`
- Modify: `firmware/test/test_port_helpers/test_main.cpp`
- Modify: `tests/test_esp32_2432s024c_contract.py`

**Interfaces:**
- Consumes: `UsageData::fable_valid`, `fable_pct`, and `fable_reset_mins` from Task 3.
- Produces: a 320x240-only compact Claude layout with three rows and a compact status label beside the page dots.

- [ ] **Step 1: Replace the old landscape layout contract with failing compact-row assertions**

Extend `UiLayoutMetrics` expectations in Unity:

```cpp
void test_320x240_layout_uses_three_compact_claude_rows(void) {
    UiLayoutMetrics m = compute_ui_layout_metrics(320, 240);
    const int third_bottom = m.claude_row_y +
        (3 * m.claude_row_h) + (2 * m.claude_row_gap);
    const int dots_right = ((m.screen_width - 31) / 2) + 31;

    TEST_ASSERT_TRUE(m.claude_compact_rows);
    TEST_ASSERT_EQUAL_INT(52, m.claude_row_y);
    TEST_ASSERT_EQUAL_INT(47, m.claude_row_h);
    TEST_ASSERT_EQUAL_INT(5, m.claude_row_gap);
    TEST_ASSERT_EQUAL_INT(8, m.claude_bar_h);
    TEST_ASSERT_EQUAL_INT(203, third_bottom);
    TEST_ASSERT_LESS_OR_EQUAL_INT(m.page_indicator_y - 8, third_bottom);
    TEST_ASSERT_LESS_THAN_INT(m.claude_status_x, dots_right);
    TEST_ASSERT_LESS_OR_EQUAL_INT(m.screen_width, m.claude_status_x + m.claude_status_w);
}
```

The final two comparisons use Unity's reversed expected/actual macro ordering: they assert `dots_right <= status_x` and `status_x + width <= screen_width`.

In `tests/test_esp32_2432s024c_contract.py`, replace `test_landscape_claude_usage_uses_second_card_coordinates` with assertions that the compact branch calls `make_compact_usage_row` three times for `Currently`, `Weekly`, and `Fable`, while the existing Codex second-card test remains unchanged.

- [ ] **Step 2: Run tests and verify the new contracts fail**

Run:

```powershell
& 'C:\Users\Gustavo\Documents\esp32-claude\.venv\Scripts\python.exe' -m pytest tests/test_esp32_2432s024c_contract.py -q
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' test -d firmware -e esp32_2432s024c -f test_port_helpers --without-uploading --without-testing
```

Expected: Python assertion failure and Unity compile failure for missing compact metrics/helper.

- [ ] **Step 3: Add exact compact layout metrics**

Add these fields to `UiLayoutMetrics` and the mirrored `Layout` struct:

```cpp
bool claude_compact_rows;
int16_t claude_row_y;
int16_t claude_row_h;
int16_t claude_row_gap;
int16_t claude_bar_h;
int16_t claude_status_x;
int16_t claude_status_y;
int16_t claude_status_w;
```

Initialize `claude_compact_rows = false` for all layouts. In the exact 320x240 branch set:

```cpp
metrics.claude_compact_rows = true;
metrics.claude_row_y = 52;
metrics.claude_row_h = 47;
metrics.claude_row_gap = 5;
metrics.claude_bar_h = 8;
metrics.claude_status_x = 184;
metrics.claude_status_y = 219;
metrics.claude_status_w = 90;
```

Copy every field from `UiLayoutMetrics` into `L` in `compute_layout`.

- [ ] **Step 4: Build a dedicated compact usage-row helper**

Add `panel_fable` and the Fable widget pointers beside the existing Claude widgets:

```cpp
static lv_obj_t* panel_fable = nullptr;
static lv_obj_t* lbl_fable_pct = nullptr;
static lv_obj_t* lbl_fable_label = nullptr;
static lv_obj_t* bar_fable = nullptr;
static lv_obj_t* lbl_fable_reset = nullptr;
```

Add this helper after `make_usage_panel`:

```cpp
static lv_obj_t* make_compact_usage_row(
    lv_obj_t* parent,
    int y,
    const char* label,
    lv_obj_t** out_pct,
    lv_obj_t** out_label,
    lv_obj_t** out_bar,
    lv_obj_t** out_reset
) {
    lv_obj_t* panel = make_panel(
        parent, L.margin, y, L.content_w, L.claude_row_h
    );
    lv_obj_set_style_pad_all(panel, 5, 0);
    const int content_width = L.content_w - 10;

    *out_label = lv_label_create(panel);
    lv_label_set_text(*out_label, label);
    lv_obj_set_style_text_font(*out_label, &font_styrene_14, 0);
    lv_obj_set_style_text_color(*out_label, COL_TEXT, 0);
    lv_obj_set_pos(*out_label, 0, 0);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_width(*out_reset, 118);
    lv_label_set_long_mode(*out_reset, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(*out_reset, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(*out_reset, &font_styrene_14, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, content_width - 166, 0);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "--");
    lv_obj_set_style_text_font(*out_pct, &font_styrene_16, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_align(*out_pct, LV_ALIGN_TOP_RIGHT, 0, -1);

    *out_bar = make_bar(panel, 0, 27, content_width, L.claude_bar_h);
    return panel;
}
```

In `init_usage_screen`, use this helper only when `L.claude_compact_rows` is true. Create rows at `row_y`, `row_y + row_h + gap`, and `row_y + 2 * (row_h + gap)` with labels `Currently`, `Weekly`, and `Fable`. Keep the existing two-card construction verbatim in the `else` branch for every other display profile.

- [ ] **Step 5: Update three rows from `UsageData`**

Add a compact reset formatter:

```cpp
static void format_compact_reset_time(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "%dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "%dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "%dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}
```

Use it for Current and Weekly when `L.claude_compact_rows`, retaining `format_reset_time` elsewhere. After updating Weekly, update Fable:

```cpp
if (panel_fable) {
    if (data->fable_valid) {
        const int f_pct = static_cast<int>(data->fable_pct + 0.5f);
        lv_label_set_text_fmt(lbl_fable_pct, "%d%%", f_pct);
        format_compact_reset_time(data->fable_reset_mins, buf, sizeof(buf));
        lv_label_set_text(lbl_fable_reset, buf);
        lv_bar_set_value(bar_fable, f_pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(
            bar_fable, pct_color(data->fable_pct), LV_PART_INDICATOR
        );
    } else {
        lv_label_set_text(lbl_fable_pct, "--");
        lv_label_set_text(lbl_fable_reset, "Unavailable");
        lv_bar_set_value(bar_fable, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar_fable, COL_MUTED, LV_PART_INDICATOR);
    }
}
```

For enterprise data on the compact profile, retain the measured Spending and Period percentages but keep the enterprise description/status overlays hidden; the three row labels become `Spending`, `Period`, and `Fable`. The existing enterprise presentation on other displays remains unchanged.

- [ ] **Step 6: Move the animated word beside the dots**

When creating `lbl_anim`, select the compact font and geometry only for 320x240:

```cpp
if (L.claude_compact_rows) {
    lv_obj_set_width(lbl_anim, L.claude_status_w);
    lv_label_set_long_mode(lbl_anim, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(lbl_anim, &font_styrene_14, 0);
    lv_obj_set_pos(lbl_anim, L.claude_status_x, L.claude_status_y);
} else {
    lv_obj_set_style_text_font(lbl_anim, L.status_font, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_TOP_MID, 0, L.footer_y);
}
```

Replace the final formatting in `ui_tick_anim` with:

```cpp
if (L.claude_compact_rows) {
    lv_label_set_text(lbl_anim, text);
} else {
    static char buf[80];
    snprintf(
        buf,
        sizeof(buf),
        "%s %s\xE2\x80\xA6",
        spinner_frames[anim_spinner_idx],
        text
    );
    lv_label_set_text(lbl_anim, buf);
}
```

This removes the glyph, ellipsis, and old full-width footer occupation only on the target layout.

- [ ] **Step 7: Run layout tests, build, and commit**

Run:

```powershell
& 'C:\Users\Gustavo\Documents\esp32-claude\.venv\Scripts\python.exe' -m pytest tests/test_esp32_2432s024c_contract.py -q
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' test -d firmware -e esp32_2432s024c -f test_port_helpers --without-uploading --without-testing
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' run -d firmware -e esp32_2432s024c_landscape
```

Expected: Python contracts pass, Unity test firmware compiles, and the landscape application build succeeds.

Commit:

```powershell
git add firmware/src/ui_layout.h firmware/src/ui.cpp firmware/test/test_port_helpers/test_main.cpp tests/test_esp32_2432s024c_contract.py
git commit -m "feat: show three Claude usage rows"
```

---

### Task 5: Document, flash, and physically verify the complete flow

**Files:**
- Modify: `README.md`
- Modify: `docs/README.pt-BR.md`

**Interfaces:**
- Consumes: completed host and firmware changes from Tasks 1-4.
- Produces: documented behavior and verified physical ESP32 output.

- [ ] **Step 1: Update public dashboard documentation**

In both README files, update the Claude page description to list `Currently`, `Weekly`, and the optional Fable scoped weekly allowance. Add one sentence beside the existing internal-format warning explaining that Fable uses Claude Code's internal OAuth usage endpoint and becomes `Unavailable` if that response changes or does not expose an active scoped limit.

- [ ] **Step 2: Run the full relevant host and contract suites**

Run:

```powershell
& 'C:\Users\Gustavo\Documents\esp32-claude\.venv\Scripts\python.exe' -m pytest daemon/tests tests/test_esp32_2432s024c_contract.py -q
```

Expected: all collected tests pass.

- [ ] **Step 3: Compile the physical Unity suite and landscape firmware**

Run:

```powershell
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' test -d firmware -e esp32_2432s024c -f test_port_helpers --without-uploading --without-testing
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' run -d firmware -e esp32_2432s024c_landscape
```

Expected: both commands exit 0.

- [ ] **Step 4: Upload to the connected board**

Confirm the CH340 port first:

```powershell
& 'C:\Users\Gustavo\Documents\esp32-claude\.venv\Scripts\python.exe' -c "import serial.tools.list_ports as p; [print(x.device, x.description) for x in p.comports()]"
```

Then upload using the detected CH340 port, expected to be COM3 on this machine:

```powershell
& 'C:\Users\Gustavo\.platformio\penv\Scripts\pio.exe' run -d firmware -e esp32_2432s024c_landscape -t upload --upload-port COM3
```

Expected: PlatformIO reports `SUCCESS` and the board reboots into the landscape dashboard.

- [ ] **Step 5: Restart the Windows tray and verify a real acknowledged payload**

Restart only the exact Clawdmeter tray process, launch it from this worktree with the project virtual environment, then inspect `%LOCALAPPDATA%\Clawdmeter\daemon.log`.

Expected evidence:

- `Clawdmeter identified on COM3`;
- a `Sending by USB serial` payload containing real `f` and `fr` values;
- no `USB serial acknowledgement failed` after the send.

- [ ] **Step 6: Inspect the physical display**

Confirm on the device:

- three full-width rows appear in order: Currently, Weekly, Fable;
- all bars are thin and readable;
- Fable matches the live OAuth percentage;
- the old lower trace is absent;
- the three page dots remain unobstructed;
- the small animated word appears immediately to their right;
- touch navigation still moves left and right;
- Codex and Activity remain unchanged.

- [ ] **Step 7: Commit documentation and record final status**

```powershell
git add README.md docs/README.pt-BR.md
git commit -m "docs: explain Fable usage metric"
git status --short --branch
```

Expected: clean working tree, with the branch ahead only by the planned feature commits.
