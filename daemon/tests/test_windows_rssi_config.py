#!/usr/bin/env python3
"""Unit tests for the Windows daemon's `rssi` config key (BLE signal bars).

The key follows the clock/chime opt-in pattern: `rssi = on|off` in the config
file, default off, adding "rb":1 to the payload only when opted in so old
firmware and unconfigured setups are unaffected.

Run: python -m pytest daemon/tests/test_windows_rssi_config.py -x -q
"""
import daemon.claude_usage_daemon_windows as mod
from daemon.claude_usage_daemon_windows import add_rssi_field, read_rssi_setting


# ---------------------------------------------------------------------------
# read_rssi_setting
# ---------------------------------------------------------------------------

def test_rssi_defaults_off_when_config_absent(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "CONFIG_FILE", tmp_path / "config")  # absent
    assert read_rssi_setting() == "off"


def test_rssi_defaults_off_when_key_absent(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("clock = auto\nchime = on\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    assert read_rssi_setting() == "off"


def test_rssi_on(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("rssi = on\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    assert read_rssi_setting() == "on"


def test_rssi_tolerates_case_whitespace_comments_and_crlf(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("  RSSI =  ON   # show the signal bars\r\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    assert read_rssi_setting() == "on"


def test_rssi_invalid_value_falls_back_to_off(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("rssi = yes\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    assert read_rssi_setting() == "off"


# ---------------------------------------------------------------------------
# add_rssi_field
# ---------------------------------------------------------------------------

def test_payload_gets_rb_when_on(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("rssi = on\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    payload = {"s": 10, "ok": True}
    add_rssi_field(payload)
    assert payload["rb"] == 1


def test_payload_untouched_when_off(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("rssi = off\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    payload = {"s": 10, "ok": True}
    add_rssi_field(payload)
    assert "rb" not in payload
    assert payload == {"s": 10, "ok": True}


def test_payload_untouched_when_config_absent(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "CONFIG_FILE", tmp_path / "config")  # absent
    payload = {"s": 10, "ok": True}
    add_rssi_field(payload)
    assert "rb" not in payload
