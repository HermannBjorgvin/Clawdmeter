#!/usr/bin/env python3
"""Unit tests for the macOS/Linux daemon's `rssi` config key (BLE signal bars).

Mirrors test_windows_rssi_config.py: `rssi = on|off`, default off, and "rb":1
added to the payload only when opted in.

Run: python -m pytest daemon/tests/test_macos_rssi_config.py -x -q
"""
import daemon.claude_usage_daemon as mod
from daemon.claude_usage_daemon import add_rssi_field, read_rssi_setting


def _cfg(tmp_path, monkeypatch, text=None):
    cfg = tmp_path / "config"
    if text is not None:
        cfg.write_text(text)
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)


def test_rssi_defaults_off_when_config_absent(tmp_path, monkeypatch):
    _cfg(tmp_path, monkeypatch)
    assert read_rssi_setting() == "off"


def test_rssi_defaults_off_when_key_absent(tmp_path, monkeypatch):
    _cfg(tmp_path, monkeypatch, "clock = auto\nchime = on\n")
    assert read_rssi_setting() == "off"


def test_rssi_on(tmp_path, monkeypatch):
    _cfg(tmp_path, monkeypatch, "rssi = on\n")
    assert read_rssi_setting() == "on"


def test_rssi_tolerates_case_whitespace_comments_and_crlf(tmp_path, monkeypatch):
    _cfg(tmp_path, monkeypatch, "  RSSI =  ON   # show the signal bars\r\n")
    assert read_rssi_setting() == "on"


def test_rssi_invalid_value_falls_back_to_off(tmp_path, monkeypatch):
    _cfg(tmp_path, monkeypatch, "rssi = yes\n")
    assert read_rssi_setting() == "off"


def test_payload_gets_rb_when_on(tmp_path, monkeypatch):
    _cfg(tmp_path, monkeypatch, "rssi = on\n")
    payload = {"s": 1, "ok": True}
    add_rssi_field(payload)
    assert payload["rb"] == 1


def test_payload_untouched_when_off(tmp_path, monkeypatch):
    _cfg(tmp_path, monkeypatch, "rssi = off\n")
    payload = {"s": 1, "ok": True}
    add_rssi_field(payload)
    assert payload == {"s": 1, "ok": True}
