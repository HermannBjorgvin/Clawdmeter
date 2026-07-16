# Multi-screen Usage Carousel Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add four touch-controlled, automatically rotating dashboard screens for Claude usage, Codex usage, local activity, and the robot animation on the USB-powered ESP32-2432S024C.

**Architecture:** The Windows serial daemon remains the only data collector and enriches the existing Claude payload with compact Codex and activity objects. The firmware parses provider updates independently, stores them in the existing dashboard state, and renders four deterministic pages driven by a small pure carousel state machine. Local collectors transmit aggregate counts only and never send prompt text, response text, file paths, or task titles.

**Tech Stack:** Python 3.13, pytest, pyserial, C++17, ArduinoJson 7, LVGL 9, PlatformIO, Unity, ESP32 Arduino framework.

## Global Constraints

- Target exactly the verified ESP32-2432S024C, 240 x 320, 4 MB flash, no PSRAM, on COM3.
- Keep USB serial at 115200 baud as the active Windows transport; do not add Wi-Fi, Bluetooth transport, OTA, or a cloud relay.
- Fixed page order: Claude -> Codex -> Activity -> Robot -> Claude.
- Automatic page interval: 12 seconds.
- A manual short touch advances exactly one page and defers the next automatic advance for 30 seconds.
- The BOOT-button brightness and robot-animation behavior must remain unchanged.
- Do not create or show a battery object when `board_caps().has_battery` is false.
- Scale the 80 x 80 logo to 48 x 48 on the 240 x 320 profile and keep it outside the metric cards.
- Preserve old Claude-only JSON payload compatibility.
- Missing provider fields must not erase the last valid data for another provider.
- Label Claude `idle` sessions as `Waiting`; label Codex unread tasks as `Unread`; never claim unread means a reply is required.
- Do not transmit prompt text, response text, project paths, session names, or task titles.
- Do not display a Codex running count unless a stable local signal is implemented and tested; this plan deliberately omits that count.
- Do not claim completion without pytest, physical Unity tests, PlatformIO build, COM3 upload, serial ACK, and visual/touch timing evidence.

---

## File structure

- Create `daemon/dashboard_collectors.py`: read-only aggregate collectors for Claude Code and Codex local state.
- Create `daemon/dashboard_payload.py`: convert normalized collector results into the compact serial schema.
- Create `daemon/tests/test_dashboard_collectors.py`: sanitized local-state fixtures and collector tests.
- Create `daemon/tests/test_dashboard_payload.py`: compact schema, privacy, optional-provider, and size tests.
- Modify `daemon/claude_usage_daemon_serial_windows.py`: enrich every serial update without changing port discovery or ACK handling.
- Modify `firmware/src/data.h`: add bounded Codex and activity fields to the existing dashboard state.
- Create `firmware/src/dashboard_payload.h` and `firmware/src/dashboard_payload.cpp`: parse old and new serial JSON independently of transport.
- Create `firmware/src/dashboard_carousel.h`: pure four-page timing and wraparound state machine.
- Modify `firmware/src/main.cpp`: use the parser, identify USB/BLE update source, and tick carousel navigation.
- Modify `firmware/src/ui_layout.h`: add explicit small-screen logo, percentage-font, footer, and page-dot metrics.
- Modify `firmware/src/ui.h` and `firmware/src/ui.cpp`: render and update four pages, page dots, freshness, and manual/automatic navigation.
- Modify `firmware/test/test_port_helpers/test_main.cpp`: physical Unity coverage for parser, timing, layout, and backward compatibility.
- Modify `tests/test_esp32_2432s024c_contract.py`: source-level guard that battery assets are conditional on board capability.
- Modify `daemon/README-windows.md`: document aggregate sources, labels, privacy, and internal-format fallback behavior.

---

### Task 1: Collect aggregate Claude Code and Codex state

**Files:**
- Create: `daemon/dashboard_collectors.py`
- Create: `daemon/tests/test_dashboard_collectors.py`

**Interfaces:**
- Consumes: `%USERPROFILE%\.claude\sessions\*.json`, `%USERPROFILE%\.codex\sessions\**\*.jsonl`, and `%USERPROFILE%\.codex\.codex-global-state.json`.
- Produces: `collect_claude_activity(claude_home: Path) -> dict[str, int]`, `collect_codex_activity(codex_home: Path) -> dict[str, int]`, and `collect_codex_usage(codex_home: Path, now: float | None = None) -> dict[str, object]`.

- [ ] **Step 1: Write failing collector tests with sanitized local files**

Create `daemon/tests/test_dashboard_collectors.py` with these cases:

```python
import json
from datetime import datetime, timezone
from pathlib import Path

from daemon.dashboard_collectors import (
    collect_claude_activity,
    collect_codex_activity,
    collect_codex_usage,
)


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value), encoding="utf-8")


def write_jsonl(path: Path, values: list[object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = "\n".join(json.dumps(value) for value in values) + "\n"
    path.write_text(text, encoding="utf-8")


def test_collect_claude_activity_counts_open_busy_and_waiting(tmp_path: Path) -> None:
    sessions = tmp_path / ".claude" / "sessions"
    write_json(sessions / "1.json", {"status": "busy"})
    write_json(sessions / "2.json", {"status": "idle"})
    write_json(sessions / "3.json", {"status": "shell"})
    (sessions / "broken.json").write_text("{", encoding="utf-8")

    assert collect_claude_activity(tmp_path / ".claude") == {
        "open": 3,
        "busy": 1,
        "waiting": 1,
    }


def test_collect_codex_activity_counts_unread_without_titles(tmp_path: Path) -> None:
    state = {
        "electron-persisted-atom-state": {
            "unread-thread-ids-by-host-v1": {"local": ["a", "b", "c"]}
        }
    }
    write_json(tmp_path / ".codex" / ".codex-global-state.json", state)

    assert collect_codex_activity(tmp_path / ".codex") == {"unread": 3}


def test_collect_codex_usage_reads_available_windows_and_daily_tokens(tmp_path: Path) -> None:
    now = datetime(2026, 7, 16, 18, 0, tzinfo=timezone.utc).timestamp()
    event = {
        "timestamp": "2026-07-16T17:55:00+00:00",
        "type": "event_msg",
        "payload": {
            "type": "token_count",
            "info": {"last_token_usage": {"total_tokens": 120}},
            "rate_limits": {
                "primary": {
                    "used_percent": 2,
                    "window_minutes": 10080,
                    "resets_at": int(now) + 600,
                },
                "secondary": None,
                "plan_type": "pro",
            },
        },
    }
    write_jsonl(
        tmp_path / ".codex" / "sessions" / "2026" / "07" / "16" / "rollout.jsonl",
        [event],
    )

    assert collect_codex_usage(tmp_path / ".codex", now=now) == {
        "limits": [{"percent": 2.0, "window_minutes": 10080, "reset_minutes": 10}],
        "tokens_today": 120,
        "plan": "pro",
    }


def test_missing_or_malformed_state_returns_empty_aggregates(tmp_path: Path) -> None:
    assert collect_claude_activity(tmp_path / ".claude") == {}
    assert collect_codex_activity(tmp_path / ".codex") == {}
    assert collect_codex_usage(tmp_path / ".codex", now=0) == {}
```

- [ ] **Step 2: Run the new tests and verify the module is missing**

Run:

```powershell
python -m pytest daemon/tests/test_dashboard_collectors.py -q
```

Expected: collection fails with `ModuleNotFoundError: No module named 'daemon.dashboard_collectors'`.

- [ ] **Step 3: Implement bounded, aggregate-only collectors**

Create `daemon/dashboard_collectors.py` with these exact public functions and rules:

```python
from __future__ import annotations

import json
import time
from datetime import datetime
from pathlib import Path
from collections.abc import Iterator
from typing import Any


def _read_json(path: Path) -> Any | None:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return None


def collect_claude_activity(claude_home: Path) -> dict[str, int]:
    sessions_dir = claude_home / "sessions"
    if not sessions_dir.is_dir():
        return {}
    statuses: list[str] = []
    for path in sessions_dir.glob("*.json"):
        value = _read_json(path)
        if isinstance(value, dict) and isinstance(value.get("status"), str):
            statuses.append(value["status"])
    return {
        "open": len(statuses),
        "busy": sum(status == "busy" for status in statuses),
        "waiting": sum(status == "idle" for status in statuses),
    }


def collect_codex_activity(codex_home: Path) -> dict[str, int]:
    value = _read_json(codex_home / ".codex-global-state.json")
    if not isinstance(value, dict):
        return {}
    atom = value.get("electron-persisted-atom-state")
    unread_by_host = atom.get("unread-thread-ids-by-host-v1") if isinstance(atom, dict) else None
    unread = unread_by_host.get("local") if isinstance(unread_by_host, dict) else None
    return {"unread": len(unread)} if isinstance(unread, list) else {}


def _iter_codex_events(path: Path) -> Iterator[dict[str, Any]]:
    try:
        with path.open("r", encoding="utf-8") as handle:
            for line in handle:
                try:
                    value = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if isinstance(value, dict):
                    yield value
    except (OSError, UnicodeError):
        return


def _parse_limit(value: object, now: float) -> dict[str, float | int] | None:
    if not isinstance(value, dict):
        return None
    percent = value.get("used_percent")
    window = value.get("window_minutes")
    reset = value.get("resets_at")
    if not isinstance(percent, (int, float)) or not isinstance(window, int):
        return None
    reset_minutes = -1
    if isinstance(reset, (int, float)):
        reset_minutes = max(0, int((float(reset) - now + 59) // 60))
    return {
        "percent": float(percent),
        "window_minutes": window,
        "reset_minutes": reset_minutes,
    }


def collect_codex_usage(codex_home: Path, now: float | None = None) -> dict[str, object]:
    scan_time = time.time() if now is None else now
    session_root = codex_home / "sessions"
    if not session_root.is_dir():
        return {}
    try:
        files = sorted(
            session_root.rglob("*.jsonl"),
            key=lambda path: path.stat().st_mtime,
            reverse=True,
        )
    except OSError:
        return {}
    latest_rate_limits: dict[str, Any] | None = None
    tokens_today = 0
    local_now = datetime.fromtimestamp(scan_time)
    local_day = local_now.date()
    day_start = local_now.replace(hour=0, minute=0, second=0, microsecond=0).timestamp()
    for path in files:
        try:
            modified_today = path.stat().st_mtime >= day_start
        except OSError:
            continue
        if latest_rate_limits is not None and not modified_today:
            continue
        file_rate_limits: dict[str, Any] | None = None
        for event in _iter_codex_events(path):
            payload = event.get("payload")
            if event.get("type") != "event_msg" or not isinstance(payload, dict):
                continue
            if payload.get("type") != "token_count":
                continue
            rate_limits = payload.get("rate_limits")
            if isinstance(rate_limits, dict):
                file_rate_limits = rate_limits
            if not modified_today:
                continue
            timestamp = event.get("timestamp")
            info = payload.get("info")
            last_usage = info.get("last_token_usage") if isinstance(info, dict) else None
            total = last_usage.get("total_tokens") if isinstance(last_usage, dict) else None
            try:
                event_day = datetime.fromisoformat(str(timestamp).replace("Z", "+00:00")).astimezone().date()
            except (TypeError, ValueError):
                continue
            if event_day == local_day and isinstance(total, int):
                tokens_today += max(0, total)
        if latest_rate_limits is None and file_rate_limits is not None:
            latest_rate_limits = file_rate_limits
    if latest_rate_limits is None:
        return {"tokens_today": tokens_today} if tokens_today else {}
    limits = [
        parsed
        for key in ("primary", "secondary")
        if (parsed := _parse_limit(latest_rate_limits.get(key), scan_time)) is not None
    ]
    result: dict[str, object] = {"limits": limits, "tokens_today": tokens_today}
    plan = latest_rate_limits.get("plan_type")
    if isinstance(plan, str):
        result["plan"] = plan[:11]
    return result
```

The implementation must not log parsed JSONL lines or retain prompt-bearing event objects beyond the function call.

- [ ] **Step 4: Run collector tests**

Run:

```powershell
python -m pytest daemon/tests/test_dashboard_collectors.py -q
```

Expected: 4 tests pass.

- [ ] **Step 5: Commit the aggregate collectors**

```powershell
git add daemon/dashboard_collectors.py daemon/tests/test_dashboard_collectors.py
git commit -m "feat: collect local Claude and Codex activity"
```

---

### Task 2: Build and send the compact dashboard payload

**Files:**
- Create: `daemon/dashboard_payload.py`
- Create: `daemon/tests/test_dashboard_payload.py`
- Modify: `daemon/claude_usage_daemon_serial_windows.py:13-21,124-151`
- Modify: `daemon/tests/test_windows_serial.py`

**Interfaces:**
- Consumes: the three collectors from Task 1 and the existing Claude payload returned by `poll_api()`.
- Produces: `build_dashboard_payload(claude_payload: dict | None, profile_dir: Path, now: float | None = None) -> dict` using top-level keys `v`, `ts`, `x`, and `a` while preserving existing Claude keys.

- [ ] **Step 1: Write failing compact-schema and privacy tests**

Create `daemon/tests/test_dashboard_payload.py`:

```python
import json
from pathlib import Path

from daemon.dashboard_payload import build_dashboard_payload


def test_build_dashboard_payload_preserves_claude_and_compacts_local_data(
    monkeypatch,
    tmp_path: Path,
) -> None:
    monkeypatch.setattr(
        "daemon.dashboard_payload.collect_codex_usage",
        lambda _home, now=None: {
            "limits": [{"percent": 2.0, "window_minutes": 10080, "reset_minutes": 10}],
            "tokens_today": 120,
            "plan": "pro",
        },
    )
    monkeypatch.setattr(
        "daemon.dashboard_payload.collect_claude_activity",
        lambda _home: {"open": 3, "busy": 1, "waiting": 1},
    )
    monkeypatch.setattr(
        "daemon.dashboard_payload.collect_codex_activity",
        lambda _home: {"unread": 5},
    )

    payload = build_dashboard_payload({"s": 41, "w": 12, "ok": True}, tmp_path, now=1000)

    assert payload == {
        "s": 41,
        "w": 12,
        "ok": True,
        "v": 2,
        "ts": 1000,
        "x": {"l": [{"p": 2.0, "wm": 10080, "rm": 10}], "td": 120, "pl": "pro"},
        "a": {"cl": {"o": 3, "b": 1, "w": 1}, "cx": {"u": 5}, "ts": 1000},
    }
    wire = json.dumps(payload, separators=(",", ":"))
    assert len(wire.encode("utf-8")) < 768
    for forbidden in ("prompt", "response", "project", "title", str(tmp_path)):
        assert forbidden not in wire


def test_local_data_is_still_sent_when_claude_poll_is_unavailable(monkeypatch, tmp_path: Path) -> None:
    monkeypatch.setattr("daemon.dashboard_payload.collect_codex_usage", lambda _home, now=None: {})
    monkeypatch.setattr(
        "daemon.dashboard_payload.collect_claude_activity",
        lambda _home: {"open": 1, "busy": 0, "waiting": 1},
    )
    monkeypatch.setattr("daemon.dashboard_payload.collect_codex_activity", lambda _home: {"unread": 0})

    payload = build_dashboard_payload(None, tmp_path, now=2000)

    assert "s" not in payload
    assert payload["a"]["cx"]["u"] == 0
```

- [ ] **Step 2: Run tests and verify the payload module is missing**

Run:

```powershell
python -m pytest daemon/tests/test_dashboard_payload.py -q
```

Expected: collection fails with `ModuleNotFoundError: No module named 'daemon.dashboard_payload'`.

- [ ] **Step 3: Implement the compact payload builder**

Create `daemon/dashboard_payload.py`:

```python
from __future__ import annotations

import time
from pathlib import Path

from daemon.dashboard_collectors import (
    collect_claude_activity,
    collect_codex_activity,
    collect_codex_usage,
)


def build_dashboard_payload(
    claude_payload: dict | None,
    profile_dir: Path,
    now: float | None = None,
) -> dict:
    scan_time = time.time() if now is None else now
    payload = dict(claude_payload or {})
    payload["v"] = 2
    payload["ts"] = int(scan_time)

    codex = collect_codex_usage(profile_dir / ".codex", now=scan_time)
    if codex:
        payload["x"] = {
            "l": [
                {"p": item["percent"], "wm": item["window_minutes"], "rm": item["reset_minutes"]}
                for item in codex.get("limits", [])[:2]
            ],
            "td": int(codex.get("tokens_today", 0)),
        }
        if plan := codex.get("plan"):
            payload["x"]["pl"] = plan

    claude_activity = collect_claude_activity(profile_dir / ".claude")
    codex_activity = collect_codex_activity(profile_dir / ".codex")
    if claude_activity or codex_activity:
        payload["a"] = {"ts": int(scan_time)}
        if claude_activity:
            payload["a"]["cl"] = {
                "o": claude_activity["open"],
                "b": claude_activity["busy"],
                "w": claude_activity["waiting"],
            }
        if codex_activity:
            payload["a"]["cx"] = {"u": codex_activity["unread"]}
    return payload
```

- [ ] **Step 4: Integrate enrichment into the serial polling loop**

In `daemon/claude_usage_daemon_serial_windows.py`, import `Path` and `build_dashboard_payload`, then replace the polling branch with this flow:

```python
claude_payload = None
token = read_token()
if not token:
    log("No token; sending local dashboard data only")
    if tray_state:
        tray_state.set_error("token expired - run claude login")
else:
    try:
        claude_payload = await poll_api(token)
    except AuthError:
        if tray_state:
            tray_state.set_error("token expired - run claude login")

payload = await asyncio.to_thread(
    build_dashboard_payload,
    claude_payload,
    Path.home(),
)
if not await asyncio.to_thread(session.write_payload, payload):
    log("USB serial acknowledgement failed; reconnecting")
    break
last_poll = time.time()
used_successfully = True
if tray_state and claude_payload is not None:
    tray_state.set_connected(last_poll)
```

Do not change `candidate_serial_ports()`, `_open_port()`, or `SerialSession.write_payload()`.

- [ ] **Step 5: Add a serial-size regression assertion and run daemon tests**

Append to `daemon/tests/test_windows_serial.py`:

```python
def test_extended_dashboard_payload_fits_firmware_command_buffer():
    payload = {
        "s": 100,
        "sr": 10080,
        "w": 100,
        "wr": 10080,
        "st": "allowed",
        "ok": True,
        "v": 2,
        "ts": 2147483647,
        "x": {
            "l": [
                {"p": 100, "wm": 10080, "rm": 10080},
                {"p": 100, "wm": 10080, "rm": 10080},
            ],
            "td": 2147483647,
            "pl": "enterprise",
        },
        "a": {"cl": {"o": 999, "b": 999, "w": 999}, "cx": {"u": 999}, "ts": 2147483647},
    }
    wire = json.dumps(payload, separators=(",", ":")).encode("utf-8") + b"\n"
    assert len(wire) < 768
```

Run:

```powershell
python -m pytest daemon/tests/test_dashboard_payload.py daemon/tests/test_windows_serial.py -q
python -m pytest daemon/tests -q
```

Expected: the focused tests pass, then the complete daemon suite passes.

- [ ] **Step 6: Commit payload integration**

```powershell
git add daemon/dashboard_payload.py daemon/claude_usage_daemon_serial_windows.py daemon/tests/test_dashboard_payload.py daemon/tests/test_windows_serial.py
git commit -m "feat: send Codex and activity dashboard data"
```

---

### Task 3: Parse provider updates independently in firmware

**Files:**
- Modify: `firmware/src/data.h`
- Create: `firmware/src/dashboard_payload.h`
- Create: `firmware/src/dashboard_payload.cpp`
- Modify: `firmware/src/main.cpp:101-151,191-225,414-422`
- Modify: `firmware/test/test_port_helpers/test_main.cpp`

**Interfaces:**
- Consumes: compact JSON keys `x.l[].p`, `x.l[].wm`, `x.l[].rm`, `x.td`, `x.pl`, `a.cl.o`, `a.cl.b`, `a.cl.w`, `a.cx.u`, `a.ts`, and all existing Claude keys.
- Produces: `uint8_t parse_dashboard_json(const char* json, UsageData* out)` returning `DASHBOARD_UPDATE_CLAUDE`, `DASHBOARD_UPDATE_CODEX`, and/or `DASHBOARD_UPDATE_ACTIVITY`.

- [ ] **Step 1: Add failing physical parser tests**

Add `#include "dashboard_payload.h"` and these tests to `firmware/test/test_port_helpers/test_main.cpp`:

```cpp
void test_old_claude_payload_remains_compatible(void) {
    UsageData data{};
    uint8_t mask = parse_dashboard_json(
        "{\"s\":12.5,\"sr\":30,\"w\":34,\"wr\":60,\"ok\":true}",
        &data
    );

    TEST_ASSERT_BITS_HIGH(DASHBOARD_UPDATE_CLAUDE, mask);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 12.5f, data.session_pct);
    TEST_ASSERT_FALSE(data.codex.valid);
    TEST_ASSERT_FALSE(data.activity.valid);
}

void test_new_payload_parses_codex_and_activity(void) {
    UsageData data{};
    uint8_t mask = parse_dashboard_json(
        "{\"v\":2,\"ts\":1000,\"x\":{\"l\":[{\"p\":2,\"wm\":10080,\"rm\":10}],\"td\":120,\"pl\":\"pro\"},\"a\":{\"cl\":{\"o\":3,\"b\":1,\"w\":1},\"cx\":{\"u\":5},\"ts\":1000}}",
        &data
    );

    TEST_ASSERT_BITS_HIGH(DASHBOARD_UPDATE_CODEX, mask);
    TEST_ASSERT_BITS_HIGH(DASHBOARD_UPDATE_ACTIVITY, mask);
    TEST_ASSERT_EQUAL_UINT8(1, data.codex.limit_count);
    TEST_ASSERT_EQUAL_INT(10080, data.codex.limits[0].window_mins);
    TEST_ASSERT_EQUAL_UINT32(120, data.codex.tokens_today);
    TEST_ASSERT_EQUAL_INT(3, data.activity.claude_open);
    TEST_ASSERT_EQUAL_INT(5, data.activity.codex_unread);
}

void test_missing_codex_window_is_not_invented_and_zero_unread_is_valid(void) {
    UsageData data{};
    uint8_t mask = parse_dashboard_json(
        "{\"x\":{\"l\":[],\"td\":0},\"a\":{\"cx\":{\"u\":0},\"ts\":1000}}",
        &data
    );

    TEST_ASSERT_BITS_HIGH(DASHBOARD_UPDATE_CODEX, mask);
    TEST_ASSERT_EQUAL_UINT8(0, data.codex.limit_count);
    TEST_ASSERT_BITS_HIGH(DASHBOARD_UPDATE_ACTIVITY, mask);
    TEST_ASSERT_TRUE(data.activity.codex_valid);
    TEST_ASSERT_EQUAL_INT(0, data.activity.codex_unread);
}
```

Register all three with `RUN_TEST` in `setup()`.

- [ ] **Step 2: Run physical Unity tests and verify the parser header is missing**

Stop the tray process holding COM3, then run:

```powershell
pio test -d firmware -e esp32_2432s024c --upload-port COM3 -f test_port_helpers
```

Expected: compilation fails because `dashboard_payload.h` does not exist.

- [ ] **Step 3: Extend the bounded firmware state**

Add these definitions before `UsageData` in `firmware/src/data.h`, then add `codex`, `activity`, `updated_epoch`, and `transport` fields at the end of `UsageData`:

```cpp
enum DashboardTransport : uint8_t {
    DASHBOARD_TRANSPORT_NONE,
    DASHBOARD_TRANSPORT_USB,
    DASHBOARD_TRANSPORT_BLE,
};

struct CodexLimitData {
    float percent;
    int window_mins;
    int reset_mins;
};

struct CodexData {
    CodexLimitData limits[2];
    uint8_t limit_count;
    uint32_t tokens_today;
    char plan[12];
    bool valid;
};

struct ActivityData {
    int claude_open;
    int claude_busy;
    int claude_waiting;
    int codex_unread;
    bool claude_valid;
    bool codex_valid;
    long scanned_epoch;
    bool valid;
};
```

The fields appended to `UsageData` are:

```cpp
    CodexData codex;
    ActivityData activity;
    long updated_epoch;
    DashboardTransport transport;
```

- [ ] **Step 4: Implement the provider-aware parser**

Create `firmware/src/dashboard_payload.h`:

```cpp
#pragma once

#include <stdint.h>
#include "data.h"

enum DashboardUpdateMask : uint8_t {
    DASHBOARD_UPDATE_NONE = 0,
    DASHBOARD_UPDATE_CLAUDE = 1,
    DASHBOARD_UPDATE_CODEX = 2,
    DASHBOARD_UPDATE_ACTIVITY = 4,
};

uint8_t parse_dashboard_json(const char* json, UsageData* out);
```

Create `firmware/src/dashboard_payload.cpp` with one `JsonDocument`, the existing Claude assignments moved intact from `main.cpp`, and these provider branches:

```cpp
#include "dashboard_payload.h"

#include <ArduinoJson.h>
#include <string.h>

uint8_t parse_dashboard_json(const char* json, UsageData* out) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) return DASHBOARD_UPDATE_NONE;

    uint8_t mask = DASHBOARD_UPDATE_NONE;
    if (!doc["s"].isNull()) {
        out->session_pct = doc["s"] | 0.0f;
        out->session_reset_mins = doc["sr"] | -1;
        out->weekly_pct = doc["w"] | 0.0f;
        out->weekly_reset_mins = doc["wr"] | -1;
        strlcpy(out->status, doc["st"] | "unknown", sizeof(out->status));
        out->chime = doc["c"] | false;
        const char* acct = doc["acct"] | "pro";
        out->enterprise = strcmp(acct, "ent") == 0;
        out->time_pct = doc["tp"] | 0;
        out->period_days = doc["pd"] | 30;
        strlcpy(out->reset_date, doc["rd"] | "", sizeof(out->reset_date));
        out->clock_epoch = doc["t"] | 0L;
        out->clock_fmt = doc["tf"] | 24;
        out->ok = doc["ok"] | false;
        out->valid = true;
        mask |= DASHBOARD_UPDATE_CLAUDE;
    }

    JsonObject codex = doc["x"].as<JsonObject>();
    if (!codex.isNull()) {
        out->codex.limit_count = 0;
        for (JsonObject limit : codex["l"].as<JsonArray>()) {
            if (out->codex.limit_count >= 2) break;
            CodexLimitData& target = out->codex.limits[out->codex.limit_count++];
            target.percent = limit["p"] | 0.0f;
            target.window_mins = limit["wm"] | 0;
            target.reset_mins = limit["rm"] | -1;
        }
        out->codex.tokens_today = codex["td"] | 0U;
        strlcpy(out->codex.plan, codex["pl"] | "", sizeof(out->codex.plan));
        out->codex.valid = true;
        mask |= DASHBOARD_UPDATE_CODEX;
    }

    JsonObject activity = doc["a"].as<JsonObject>();
    if (!activity.isNull()) {
        JsonObject claude = activity["cl"].as<JsonObject>();
        if (!claude.isNull()) {
            out->activity.claude_open = claude["o"] | 0;
            out->activity.claude_busy = claude["b"] | 0;
            out->activity.claude_waiting = claude["w"] | 0;
            out->activity.claude_valid = true;
        }
        JsonObject codex_activity = activity["cx"].as<JsonObject>();
        if (!codex_activity.isNull()) {
            out->activity.codex_unread = codex_activity["u"] | 0;
            out->activity.codex_valid = true;
        }
        out->activity.scanned_epoch = activity["ts"] | 0L;
        out->activity.valid = out->activity.claude_valid || out->activity.codex_valid;
        mask |= DASHBOARD_UPDATE_ACTIVITY;
    }
    out->updated_epoch = doc["ts"] | out->updated_epoch;
    return mask;
}
```

- [ ] **Step 5: Replace `main.cpp` parsing without changing usage-rate behavior**

Include `dashboard_payload.h`, remove the local `parse_json()` function, and remove the direct ArduinoJson include if no other code in `main.cpp` needs it. Change `apply_usage_json` to accept the source and run usage-rate sampling only for Claude updates:

```cpp
static bool apply_usage_json(const char* json, DashboardTransport transport) {
    uint8_t updates = parse_dashboard_json(json, &usage);
    if (updates == DASHBOARD_UPDATE_NONE) return false;
    usage.transport = transport;

    if (updates & DASHBOARD_UPDATE_CLAUDE) {
        int g_before = usage_rate_group();
        bool session_reset = usage_rate_sample(usage.session_pct);
        int g_after = usage_rate_group();
        if (session_reset && usage.chime) sound_hal_play_reset();
        if (g_after != g_before && splash_is_active()) splash_pick_for_current_rate();
    }
    ui_update(&usage);
    return true;
}
```

Use `DASHBOARD_TRANSPORT_USB` in `check_serial_cmd()` and `DASHBOARD_TRANSPORT_BLE` in the BLE data branch. Increase `CMD_BUF_SIZE` from `384` to `768`.

- [ ] **Step 6: Run physical parser tests and build**

```powershell
pio test -d firmware -e esp32_2432s024c --upload-port COM3 -f test_port_helpers
pio run -d firmware -e esp32_2432s024c
```

Expected: all Unity tests pass and the firmware build exits 0.

- [ ] **Step 7: Commit parser and data contract**

```powershell
git add firmware/src/data.h firmware/src/dashboard_payload.h firmware/src/dashboard_payload.cpp firmware/src/main.cpp firmware/test/test_port_helpers/test_main.cpp
git commit -m "feat: parse multi-provider dashboard payloads"
```

---

### Task 4: Add deterministic touch and automatic carousel timing

**Files:**
- Create: `firmware/src/dashboard_carousel.h`
- Modify: `firmware/src/ui.h`
- Modify: `firmware/src/ui.cpp:164,240-241,658-698`
- Modify: `firmware/src/main.cpp:332-395`
- Modify: `firmware/test/test_port_helpers/test_main.cpp`

**Interfaces:**
- Produces: `DashboardPage`, `CarouselState`, `carousel_start()`, `carousel_manual_next()`, and `carousel_tick()`.
- UI produces: `ui_start_dashboard(uint32_t now_ms)` and `ui_tick_navigation(uint32_t now_ms)`.

- [ ] **Step 1: Write failing carousel state tests**

Add `#include "dashboard_carousel.h"` and these tests:

```cpp
void test_carousel_wraps_in_approved_order(void) {
    CarouselState state{};
    carousel_start(state, DASHBOARD_CLAUDE, 1000);
    TEST_ASSERT_EQUAL(DASHBOARD_CODEX, carousel_manual_next(state, 2000));
    TEST_ASSERT_EQUAL(DASHBOARD_ACTIVITY, carousel_manual_next(state, 3000));
    TEST_ASSERT_EQUAL(DASHBOARD_ROBOT, carousel_manual_next(state, 4000));
    TEST_ASSERT_EQUAL(DASHBOARD_CLAUDE, carousel_manual_next(state, 5000));
}

void test_carousel_auto_advances_after_twelve_seconds(void) {
    CarouselState state{};
    carousel_start(state, DASHBOARD_CLAUDE, 1000);
    TEST_ASSERT_FALSE(carousel_tick(state, 12999));
    TEST_ASSERT_TRUE(carousel_tick(state, 13000));
    TEST_ASSERT_EQUAL(DASHBOARD_CODEX, state.page);
}

void test_manual_touch_defers_auto_advance_for_thirty_seconds(void) {
    CarouselState state{};
    carousel_start(state, DASHBOARD_CLAUDE, 1000);
    carousel_manual_next(state, 5000);
    TEST_ASSERT_FALSE(carousel_tick(state, 34999));
    TEST_ASSERT_TRUE(carousel_tick(state, 35000));
    TEST_ASSERT_EQUAL(DASHBOARD_ACTIVITY, state.page);
}
```

Register the tests in `setup()`.

- [ ] **Step 2: Run Unity and verify the carousel header is missing**

```powershell
pio test -d firmware -e esp32_2432s024c --upload-port COM3 -f test_port_helpers
```

Expected: compilation fails because `dashboard_carousel.h` does not exist.

- [ ] **Step 3: Implement the pure carousel state machine**

Create `firmware/src/dashboard_carousel.h`:

```cpp
#pragma once

#include <stdint.h>

constexpr uint32_t DASHBOARD_AUTO_MS = 12000;
constexpr uint32_t DASHBOARD_MANUAL_HOLDOFF_MS = 30000;

enum DashboardPage : uint8_t {
    DASHBOARD_CLAUDE,
    DASHBOARD_CODEX,
    DASHBOARD_ACTIVITY,
    DASHBOARD_ROBOT,
    DASHBOARD_PAGE_COUNT,
};

struct CarouselState {
    DashboardPage page;
    uint32_t next_advance_ms;
    bool started;
};

inline bool dashboard_time_reached(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}

inline DashboardPage dashboard_next_page(DashboardPage page) {
    return static_cast<DashboardPage>((static_cast<uint8_t>(page) + 1) % DASHBOARD_PAGE_COUNT);
}

inline void carousel_start(CarouselState& state, DashboardPage page, uint32_t now) {
    state.page = page;
    state.next_advance_ms = now + DASHBOARD_AUTO_MS;
    state.started = true;
}

inline DashboardPage carousel_manual_next(CarouselState& state, uint32_t now) {
    state.page = dashboard_next_page(state.page);
    state.next_advance_ms = now + DASHBOARD_MANUAL_HOLDOFF_MS;
    state.started = true;
    return state.page;
}

inline bool carousel_tick(CarouselState& state, uint32_t now) {
    if (!state.started || !dashboard_time_reached(now, state.next_advance_ms)) return false;
    state.page = dashboard_next_page(state.page);
    state.next_advance_ms = now + DASHBOARD_AUTO_MS;
    return true;
}
```

- [ ] **Step 4: Replace splash-toggle navigation with carousel navigation**

In `ui.h`, include `dashboard_carousel.h`, replace `screen_t` with `DashboardPage`, remove `ui_toggle_splash()`, and declare:

```cpp
void ui_show_screen(DashboardPage page);
void ui_start_dashboard(uint32_t now_ms);
void ui_tick_navigation(uint32_t now_ms);
DashboardPage ui_get_current_screen(void);
```

In `ui.cpp`, keep one `CarouselState` and one `DashboardPage current_page`. The global click handler initializes an unstarted carousel from the visible page before advancing, so a pre-data touch on Robot goes to Claude rather than Codex:

```cpp
static void global_click_cb(lv_event_t* event) {
    (void)event;
    uint32_t now = millis();
    if (!carousel.started) carousel_start(carousel, current_page, now);
    ui_show_screen(carousel_manual_next(carousel, now));
}
```

`ui_tick_navigation()` calls `carousel_tick()` and shows the new page only when it returns true.

The first valid payload from either USB or BLE calls `ui_start_dashboard(millis())`, which starts at `DASHBOARD_CLAUDE`. Remove `serial_usage_screen_shown` and add this immediately after `ui_update(&usage)` in `apply_usage_json()`:

```cpp
static bool dashboard_started = false;
if (!dashboard_started) {
    ui_start_dashboard(millis());
    dashboard_started = true;
}
```

Before the first payload, `setup()` shows `DASHBOARD_ROBOT` without starting the timer.

In `main.cpp`, call `ui_tick_navigation(millis())` once per loop after `ui_tick_anim()`. Keep BOOT behavior exactly:

```cpp
if (ui_get_current_screen() == DASHBOARD_ROBOT) splash_next();
else brightness_cycle();
```

- [ ] **Step 5: Run carousel tests and build**

```powershell
pio test -d firmware -e esp32_2432s024c --upload-port COM3 -f test_port_helpers
pio run -d firmware -e esp32_2432s024c
```

Expected: all tests pass and the build exits 0.

- [ ] **Step 6: Commit navigation state**

```powershell
git add firmware/src/dashboard_carousel.h firmware/src/ui.h firmware/src/ui.cpp firmware/src/main.cpp firmware/test/test_port_helpers/test_main.cpp
git commit -m "feat: add touch and automatic page carousel"
```

---

### Task 5: Fix the 240 x 320 header, panels, and battery capability

**Files:**
- Modify: `firmware/src/ui_layout.h`
- Modify: `firmware/src/ui.cpp:67-81,393-487,658-663,710-726`
- Modify: `firmware/test/test_port_helpers/test_main.cpp`
- Modify: `tests/test_esp32_2432s024c_contract.py`

**Interfaces:**
- `UiLayoutMetrics` adds `logo_size`, `logo_scale`, `percentage_font_px`, `footer_y`, and `page_indicator_y`.
- The small profile uses logo size 48, scale 153, content y 64, card height 88, percentage font 24, footer y 276, and page dots y 304.

- [ ] **Step 1: Tighten the failing small-layout test**

Replace `test_240x320_layout_fits_both_panels` with:

```cpp
void test_240x320_layout_reserves_header_cards_and_footer(void) {
    UiLayoutMetrics metrics = compute_ui_layout_metrics(240, 320);
    const int cards_bottom = metrics.content_y +
        (2 * metrics.usage_panel_h) + metrics.usage_panel_gap;

    TEST_ASSERT_TRUE(metrics.small_display);
    TEST_ASSERT_EQUAL_INT(48, metrics.logo_size);
    TEST_ASSERT_EQUAL_INT(153, metrics.logo_scale);
    TEST_ASSERT_EQUAL_INT(24, metrics.percentage_font_px);
    TEST_ASSERT_LESS_OR_EQUAL_INT(metrics.footer_y, cards_bottom);
    TEST_ASSERT_LESS_THAN_INT(metrics.screen_height, metrics.page_indicator_y);
}
```

Append this source contract to `tests/test_esp32_2432s024c_contract.py`:

```python
def test_battery_ui_is_guarded_by_board_capability():
    ui = (ROOT / "firmware" / "src" / "ui.cpp").read_text(encoding="utf-8")
    assert "if (board_caps().has_battery)" in ui
    assert "init_battery_icons();" in ui
```

- [ ] **Step 2: Run focused tests and confirm failure**

```powershell
python -m pytest tests/test_esp32_2432s024c_contract.py -q
pio test -d firmware -e esp32_2432s024c --upload-port COM3 -f test_port_helpers
```

Expected: the source contract fails because battery initialization is unconditional, and Unity fails because the new layout fields do not exist.

- [ ] **Step 3: Add explicit small-display dimensions**

Extend `UiLayoutMetrics` and set the `height <= 320` branch to:

```cpp
metrics.margin = 10;
metrics.title_y = 12;
metrics.content_y = 64;
metrics.usage_panel_h = 88;
metrics.usage_panel_gap = 8;
metrics.usage_bar_y = 36;
metrics.usage_reset_y = 61;
metrics.logo_size = 48;
metrics.logo_scale = 153;
metrics.percentage_font_px = 24;
metrics.footer_y = 276;
metrics.page_indicator_y = 304;
metrics.bluetooth_panel_h = 92;
metrics.bluetooth_reset_zone_h = 65;
metrics.small_display = true;
metrics.status_font_px = 14;
metrics.idle_creature_size = 92;
```

Set large/compact defaults to logo size 80, scale 256, percentage font 48, footer y `height - 45`, and page indicator y `height - 16` so existing boards retain their appearance.

- [ ] **Step 4: Apply the layout and make battery creation capability-driven**

In `ui.cpp`:

- select `font_styrene_24` when `L.percentage_font_px == 24`;
- call `lv_image_set_scale(logo_img, L.logo_scale)` and position the logo at `(L.margin, 6)` on the small profile;
- align the provider title at `L.title_y` without the previous 16-pixel horizontal offset;
- create and initialize battery descriptors only inside this exact guard:

```cpp
if (board_caps().has_battery) {
    init_battery_icons();
    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    lv_obj_set_pos(battery_img, L.scr_w - 48 - L.margin, L.title_y);
}
```

Keep every battery function null-safe. Do not allocate a hidden battery object on ESP32-2432S024C.

- [ ] **Step 5: Run layout tests and build**

```powershell
python -m pytest tests/test_esp32_2432s024c_contract.py -q
pio test -d firmware -e esp32_2432s024c --upload-port COM3 -f test_port_helpers
pio run -d firmware -e esp32_2432s024c
```

Expected: host contract and Unity tests pass; firmware builds successfully.

- [ ] **Step 6: Commit the physical layout correction**

```powershell
git add firmware/src/ui_layout.h firmware/src/ui.cpp firmware/test/test_port_helpers/test_main.cpp tests/test_esp32_2432s024c_contract.py
git commit -m "fix: fit dashboard header on 240x320 display"
```

---

### Task 6: Render Codex, Activity, Robot status, and page indicators

**Files:**
- Modify: `firmware/src/ui.cpp`
- Modify: `firmware/src/ui.h`
- Modify: `firmware/test/test_port_helpers/test_main.cpp`

**Interfaces:**
- Consumes: `UsageData.codex`, `UsageData.activity`, `UsageData.transport`, and carousel page state.
- Produces: visible Claude, Codex, Activity, and Robot pages; `ui_update()` updates every provider without changing the selected page.

- [ ] **Step 1: Add a failing label-contract helper test**

Add pure formatting helpers to the planned public surface in `ui.h`:

```cpp
const char* codex_window_label(int window_mins);
void format_compact_tokens(uint32_t tokens, char* buffer, size_t length);
```

Add tests:

```cpp
void test_codex_window_labels_follow_actual_window_duration(void) {
    TEST_ASSERT_EQUAL_STRING("5 hours", codex_window_label(300));
    TEST_ASSERT_EQUAL_STRING("Weekly", codex_window_label(10080));
    TEST_ASSERT_EQUAL_STRING("Limit", codex_window_label(1440));
}

void test_daily_tokens_are_formatted_compactly(void) {
    char buffer[16];
    format_compact_tokens(12500, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_STRING("12.5k", buffer);
}
```

Register both tests.

- [ ] **Step 2: Run Unity and verify the helpers are undefined**

```powershell
pio test -d firmware -e esp32_2432s024c --upload-port COM3 -f test_port_helpers
```

Expected: linking or compilation fails because the formatting helpers are absent.

- [ ] **Step 3: Implement exact provider labels and token formatting**

Add to `ui.cpp`:

```cpp
const char* codex_window_label(int window_mins) {
    if (window_mins == 300) return "5 hours";
    if (window_mins == 10080) return "Weekly";
    return "Limit";
}

void format_compact_tokens(uint32_t tokens, char* buffer, size_t length) {
    if (tokens >= 1000000U) {
        snprintf(buffer, length, "%.1fM", tokens / 1000000.0f);
    } else if (tokens >= 1000U) {
        snprintf(buffer, length, "%.1fk", tokens / 1000.0f);
    } else {
        snprintf(buffer, length, "%lu", static_cast<unsigned long>(tokens));
    }
}
```

- [ ] **Step 4: Build the Codex and Activity page objects**

In `ui.cpp`, keep the existing Claude container and add one full-screen transparent container per new metric page. Reuse `make_usage_panel()` for Codex:

- first card: the first available rate-limit window;
- second card: the second available rate-limit window, or `Tokens today` when only one window exists;
- no windows: first card reads `Codex / No limit data`, second card still shows daily tokens when `codex.valid` is true.

Activity uses two panels with these exact compact rows:

```text
Claude Code
Open     <count>
Busy     <count>
Waiting  <count>

Codex
Unread   <count>
```

If a provider is unavailable, render `Unavailable`; zero remains `0`.

Create one persistent page-indicator group at `L.page_indicator_y` containing four 5-pixel circles separated by 8 pixels. Hide the group on `DASHBOARD_ROBOT`; on the other pages color only the active dot with `COL_TEXT` and use `COL_MUTED` for the rest.

- [ ] **Step 5: Update all pages from one validated state**

Extend `ui_update(const UsageData* data)` so it:

- retains the existing Claude update behavior only when `data->valid` is true;
- updates Codex labels only when `data->codex.valid` is true;
- updates Activity labels only when `data->activity.valid` is true;
- records `millis()` as the last received update time;
- records `data->transport` for Robot status;
- never calls `ui_show_screen()`.

On the Robot page, refresh one status label from `ui_tick_anim()` no more than once per second:

```text
USB data · just now
USB data · 2m ago
BLE data · 3m ago
No data
```

Use `USB data`, not `USB connected`, because firmware can prove receipt of serial data but not the continued ownership of COM3 between updates.

- [ ] **Step 6: Wire visibility for all four pages**

`ui_show_screen()` must hide all metric containers and the splash root before this switch:

```cpp
switch (page) {
case DASHBOARD_CLAUDE:
    lv_obj_clear_flag(claude_container, LV_OBJ_FLAG_HIDDEN);
    break;
case DASHBOARD_CODEX:
    lv_obj_clear_flag(codex_container, LV_OBJ_FLAG_HIDDEN);
    break;
case DASHBOARD_ACTIVITY:
    lv_obj_clear_flag(activity_container, LV_OBJ_FLAG_HIDDEN);
    break;
case DASHBOARD_ROBOT:
    splash_show();
    break;
default:
    break;
}
```

Show the logo on the three metric pages, hide it on Robot, and update the indicator group after changing the page.

- [ ] **Step 7: Run all host and firmware tests**

```powershell
python -m pytest daemon/tests tests -q
pio test -d firmware -e esp32_2432s024c --upload-port COM3 -f test_port_helpers
pio run -d firmware -e esp32_2432s024c
```

Expected: all Python and Unity tests pass and the firmware builds.

- [ ] **Step 8: Commit complete page rendering**

```powershell
git add firmware/src/ui.h firmware/src/ui.cpp firmware/test/test_port_helpers/test_main.cpp
git commit -m "feat: render Codex and activity dashboard pages"
```

---

### Task 7: Document, flash, and validate the physical dashboard

**Files:**
- Modify: `daemon/README-windows.md`
- No additional source files.

**Interfaces:**
- Consumes: completed daemon, payload, parser, carousel, and UI tasks.
- Produces: verified COM3 firmware plus a restarted Windows tray daemon using the enriched payload.

- [ ] **Step 1: Document the new local aggregate sources**

Add a `Dashboard data` section to `daemon/README-windows.md` stating:

- Claude rate limits still come from the existing authenticated poll.
- Claude Code activity reads only aggregate `status` fields from `.claude\sessions`.
- Codex usage reads aggregate `token_count` events and rate-limit windows from `.codex\sessions`.
- Codex unread count comes from `.codex-global-state.json`.
- `Waiting` means Claude CLI status `idle`; `Unread` does not necessarily mean a reply is required.
- Codex local JSON is an internal format; a schema mismatch displays unavailable values and leaves Claude working.
- Prompt text, responses, paths, and task titles are never included in the serial payload.

- [ ] **Step 2: Run the complete automated verification set**

Stop the running tray process so COM3 is free, then run:

```powershell
python -m pytest daemon/tests tests -q
pio test -d firmware -e esp32_2432s024c --upload-port COM3 -f test_port_helpers
pio run -d firmware -e esp32_2432s024c
```

Expected: every command exits 0. Record exact pass counts and firmware RAM/flash usage.

- [ ] **Step 3: Upload the production firmware to COM3**

```powershell
pio run -d firmware -e esp32_2432s024c -t upload --upload-port COM3
```

Expected: esptool identifies the verified ESP32, writes successfully, verifies flash, and resets the board.

- [ ] **Step 4: Restart the hidden tray daemon**

Use the existing installed environment and hidden-window requirement:

```powershell
$pythonw = Resolve-Path '.venv\Scripts\pythonw.exe'
Start-Process -FilePath $pythonw -ArgumentList '-m','daemon.tray_windows' -WorkingDirectory (Get-Location) -WindowStyle Hidden
```

Expected: one tray process owns COM3 and `%LOCALAPPDATA%\Clawdmeter\daemon.log` records device identification, compact payload transmission, and a positive ACK.

- [ ] **Step 5: Compare live aggregates with local source state**

Run the collectors directly without printing raw session content:

```powershell
@'
from pathlib import Path
from daemon.dashboard_collectors import collect_claude_activity, collect_codex_activity, collect_codex_usage
home = Path.home()
print("Claude", collect_claude_activity(home / ".claude"))
print("Codex activity", collect_codex_activity(home / ".codex"))
print("Codex usage", collect_codex_usage(home / ".codex"))
'@ | python -
```

Expected: the Activity and Codex screens show the same aggregate numbers.

- [ ] **Step 6: Perform timed physical acceptance**

Verify on the actual display:

1. Robot remains visible with `No data` before the first payload.
2. The first valid payload selects Claude.
3. The 48-pixel logo is fully visible and does not overlap the first card.
4. No battery indicator appears.
5. Claude, Codex, and Activity content fits without clipping.
6. Untouched pages advance every 12 seconds in the approved order.
7. One short touch advances exactly one page.
8. After a touch, no automatic transition occurs before 30 seconds; one occurs at 30 seconds.
9. BOOT cycles brightness on metric pages and advances robot animation on Robot.
10. Orientation remains unmirrored and the robot remains orange.

Do not mark this step complete from build output; obtain visual confirmation from Gustavo if the screen cannot be observed directly.

- [ ] **Step 7: Commit documentation and any verification-only adjustments**

Only if physical validation required no unplanned source change:

```powershell
git add daemon/README-windows.md
git commit -m "docs: describe multi-source dashboard data"
```

If physical validation exposes a defect, return to the failing task, add a regression test, implement the smallest correction, rerun the complete verification set, and then commit that correction separately.

---

## Final verification checklist

- [ ] `python -m pytest daemon/tests tests -q` passes.
- [ ] `pio test -d firmware -e esp32_2432s024c --upload-port COM3 -f test_port_helpers` passes on the physical board.
- [ ] `pio run -d firmware -e esp32_2432s024c` succeeds.
- [ ] Production upload to COM3 succeeds and verifies flash.
- [ ] Serial daemon identifies the board and receives `{"ack":true}`.
- [ ] Claude usage matches the existing collector.
- [ ] Codex shows only windows actually exposed by local state.
- [ ] Activity distinguishes Claude `Waiting` and Codex `Unread`.
- [ ] No battery indicator, clipped header, mirrored image, or wrong color remains.
- [ ] Touch order, 12-second automatic interval, and 30-second manual holdoff are physically confirmed.
- [ ] Worktree contains no unrelated changes.
