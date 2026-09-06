#!/usr/bin/env python3
"""Tests for the daemon's History integration: config key, payload merge, and
the long-write path that larger payloads need over BLE.

Run: python -m pytest daemon/tests/test_history_daemon.py -x -q
"""
import asyncio
import json
from pathlib import Path
from unittest.mock import AsyncMock, MagicMock, patch

import pytest

import daemon.claude_usage_daemon as mod
from daemon.claude_usage_daemon import Session, attach_history, read_history_setting


def _run(coro):
    return asyncio.run(coro)


# ---------------------------------------------------------------------------
# config
# ---------------------------------------------------------------------------

def test_history_defaults_on_when_config_absent(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "CONFIG_FILE", tmp_path / "config")
    assert read_history_setting() == "on"


def test_history_defaults_on_when_key_absent(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("clock = auto\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    assert read_history_setting() == "on"


@pytest.mark.parametrize("raw,expected", [
    ("history = off", "off"),
    ("history = ON  # shout", "on"),
    ("history = maybe", "on"),      # unrecognised → default
])
def test_history_setting_parses(tmp_path, monkeypatch, raw, expected):
    cfg = tmp_path / "config"
    cfg.write_text(raw + "\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    assert read_history_setting() == expected


# ---------------------------------------------------------------------------
# attach_history
# ---------------------------------------------------------------------------

def _fake_history(fields: dict | None = None, fail: Exception | None = None):
    h = MagicMock()
    h.scan = MagicMock(side_effect=fail) if fail else MagicMock()
    h.save = MagicMock()
    h.payload_fields = MagicMock(return_value=fields or {"h": [1], "ht": [2], "hw": 3, "hm": []})
    h.week_start_index = MagicMock(return_value=None)   # tests opt in explicitly
    h.current_cell = MagicMock(return_value=None)      # tests opt in explicitly
    h.last_scan_files = 0
    h.last_scan_bytes = 0
    return h


def test_attach_history_merges_fields_when_on(monkeypatch):
    monkeypatch.setattr(mod, "read_history_setting", lambda: "on")
    fake = _fake_history()
    monkeypatch.setattr(mod, "_history", lambda: fake)
    payload = {"s": 10, "ok": True}
    _run(attach_history(payload))
    assert payload == {"s": 10, "ok": True, "h": [1], "ht": [2], "hw": 3, "hm": []}
    fake.scan.assert_called_once()
    fake.save.assert_called_once()


def test_attach_history_noop_when_off(monkeypatch):
    monkeypatch.setattr(mod, "read_history_setting", lambda: "off")
    fake = _fake_history()
    monkeypatch.setattr(mod, "_history", lambda: fake)
    payload = {"s": 10}
    _run(attach_history(payload))
    assert payload == {"s": 10}
    fake.scan.assert_not_called()


def test_attach_history_never_breaks_the_poll(monkeypatch):
    # A scan blowing up (permissions, disk) must not cost the usage numbers.
    monkeypatch.setattr(mod, "read_history_setting", lambda: "on")
    fake = _fake_history(fail=RuntimeError("boom"))
    monkeypatch.setattr(mod, "_history", lambda: fake)
    payload = {"s": 10}
    _run(attach_history(payload))
    assert payload == {"s": 10}


def test_attach_history_attaches_to_no_data_beat(monkeypatch):
    # Local history is available even when the OAuth token is dead.
    monkeypatch.setattr(mod, "read_history_setting", lambda: "on")
    fake = _fake_history({"h": [5], "ht": [1], "hw": 0, "hm": []})
    monkeypatch.setattr(mod, "_history", lambda: fake)
    payload = {"ok": False}
    _run(attach_history(payload))
    assert payload["ok"] is False and payload["h"] == [5]


def test_history_singleton_uses_configured_dirs_and_state_file(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [tmp_path / "a", tmp_path / "b"])
    monkeypatch.setattr(mod, "HISTORY_STATE_FILE", tmp_path / "state.json")
    monkeypatch.setattr(mod, "_HISTORY", None)
    h = mod._history()
    assert h.config_dirs == [tmp_path / "a", tmp_path / "b"]
    assert h.state_file == tmp_path / "state.json"
    assert mod._history() is h


# ---------------------------------------------------------------------------
# Session.write_payload — short payloads keep the old write-without-response
# path; anything over the MTU-safe threshold goes as a GATT long write.
# ---------------------------------------------------------------------------

def _session():
    client = MagicMock()
    client.write_gatt_char = AsyncMock()
    return Session(client), client


def test_short_payload_uses_write_without_response():
    s, client = _session()
    ok = _run(s.write_payload({"s": 1, "ok": True}))
    assert ok
    args, kwargs = client.write_gatt_char.call_args
    assert kwargs["response"] is False


def test_long_payload_uses_write_with_response():
    s, client = _session()
    payload = {"s": 1, "ok": True, "h": list(range(100, 114)), "ht": list(range(1000, 1014)),
               "hw": 6, "hm": [["Opus", 71], ["Sonnet", 19], ["Fable", 10]], "t": 1_700_000_000, "tf": 24}
    data = json.dumps(payload, separators=(",", ":")).encode()
    assert len(data) > mod.WRITE_NR_MAX_BYTES
    ok = _run(s.write_payload(payload))
    assert ok
    args, kwargs = client.write_gatt_char.call_args
    assert kwargs["response"] is True
    assert args[1] == data


def test_threshold_is_below_any_negotiated_mtu():
    # The existing ~105-byte payload has always fit as a write-without-response;
    # keep the switch-over comfortably under the 185-byte MTU macOS negotiates
    # without data-length extension (185 - 3 = 182 usable).
    assert 130 <= mod.WRITE_NR_MAX_BYTES <= 182


def test_long_write_timeout_reports_failure():
    s, client = _session()

    async def never(*_a, **_k):
        await asyncio.sleep(3600)

    client.write_gatt_char = never
    with patch.object(mod, "LONG_WRITE_TIMEOUT", 0.01):
        ok = _run(s.write_payload({"pad": "x" * 300}))
    assert ok is False


def test_write_failure_reports_false():
    s, client = _session()
    client.write_gatt_char = AsyncMock(side_effect=mod.BleakError("nope"))
    assert _run(s.write_payload({"s": 1})) is False


def test_attach_history_adds_window_start_when_weekly_reset_known(monkeypatch):
    monkeypatch.setattr(mod, "read_history_setting", lambda: "on")
    fake = _fake_history()
    fake.week_start_index = MagicMock(return_value=9)
    monkeypatch.setattr(mod, "_history", lambda: fake)
    payload = {"s": 10, "wr": 4285, "ok": True}
    _run(attach_history(payload))
    assert payload["hs"] == 9
    fake.week_start_index.assert_called_once_with(4285)


def test_attach_history_omits_window_start_without_reset(monkeypatch):
    monkeypatch.setattr(mod, "read_history_setting", lambda: "on")
    fake = _fake_history()
    fake.week_start_index = MagicMock(return_value=None)
    monkeypatch.setattr(mod, "_history", lambda: fake)
    payload = {"ok": False}                      # no-data beat carries no wr
    _run(attach_history(payload))
    assert "hs" not in payload


def test_attach_history_records_the_live_window_observation(monkeypatch):
    monkeypatch.setattr(mod, "read_history_setting", lambda: "on")
    fake = _fake_history()
    fake.observe = MagicMock()
    monkeypatch.setattr(mod, "_history", lambda: fake)
    _run(attach_history({"s": 55, "sr": 58, "ok": True}))
    fake.observe.assert_called_once_with(55, 58)


def test_no_data_beat_records_nothing(monkeypatch):
    monkeypatch.setattr(mod, "read_history_setting", lambda: "on")
    fake = _fake_history()
    fake.observe = MagicMock()
    monkeypatch.setattr(mod, "_history", lambda: fake)
    _run(attach_history({"ok": False}))
    fake.observe.assert_called_once_with(None, None)   # guarded inside observe()


def test_attach_history_marks_the_current_cell(monkeypatch):
    monkeypatch.setattr(mod, "read_history_setting", lambda: "on")
    fake = _fake_history()
    fake.current_cell = MagicMock(return_value=31)
    monkeypatch.setattr(mod, "_history", lambda: fake)
    payload = {"s": 55, "sr": 58, "ok": True}
    _run(attach_history(payload))
    assert payload["wc"] == 31
    fake.current_cell.assert_called_once_with(58)


def test_attach_history_omits_current_cell_when_unplaceable(monkeypatch):
    monkeypatch.setattr(mod, "read_history_setting", lambda: "on")
    fake = _fake_history()
    fake.current_cell = MagicMock(return_value=None)
    monkeypatch.setattr(mod, "_history", lambda: fake)
    payload = {"ok": False}
    _run(attach_history(payload))
    assert "wc" not in payload
