#!/usr/bin/env python3
"""Tests for the macOS daemon's configurable poll interval and heartbeat replay.

Mirrors the Linux daemon's `poll_interval` / `heartbeat_interval` semantics.

Run: python -m pytest daemon/tests/test_macos_poll_interval.py -x -q
"""
import pytest

import daemon.claude_usage_daemon as mod


@pytest.fixture
def config(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    return cfg


def test_intervals_default_to_60_without_config(config):
    assert mod.read_poll_interval() == 60
    assert mod.read_heartbeat_interval() == 60


def test_intervals_read_from_config(config):
    config.write_text("clock = 24\npoll_interval = 300  # five minutes\nheartbeat_interval = 45\n")
    assert mod.read_poll_interval() == 300
    assert mod.read_heartbeat_interval() == 45


def test_intervals_clamped_to_minimum(config):
    config.write_text("poll_interval = 2\nheartbeat_interval = 0\n")
    assert mod.read_poll_interval() == 10
    assert mod.read_heartbeat_interval() == 10


@pytest.mark.parametrize("raw", ["fast", "-5", "1.5", ""])
def test_invalid_interval_falls_back_to_default(config, raw):
    config.write_text(f"poll_interval = {raw}\n")
    assert mod.read_poll_interval() == 60


def test_commented_out_interval_is_ignored(config):
    config.write_text("# poll_interval = 300\n")
    assert mod.read_poll_interval() == 60


def test_age_payload_ticks_countdowns_and_clock():
    p = {"s": 40, "sr": 95, "w": 10, "wr": 3000, "t": 1000, "ok": True}
    aged = mod.age_payload(p, 150)
    assert aged == {"s": 40, "sr": 93, "w": 10, "wr": 2998, "t": 1150, "ok": True}
    assert p["sr"] == 95   # original left untouched


def test_age_payload_never_goes_negative_and_skips_absent_fields():
    aged = mod.age_payload({"sr": 1, "ok": True}, 600)
    assert aged == {"sr": 0, "ok": True}


def test_next_action_polls_on_interval_or_refresh():
    assert mod.next_action(300, 0, 0, 300, 60, True, False) == "poll"
    assert mod.next_action(10, 0, 0, 300, 60, True, True) == "poll"


def test_next_action_heartbeats_between_polls():
    assert mod.next_action(61, 0, 0, 300, 60, True, False) == "heartbeat"
    assert mod.next_action(100, 0, 61, 300, 60, True, False) is None   # replayed 39s ago


def test_next_action_no_heartbeat_without_payload_or_when_disabled():
    assert mod.next_action(61, 0, 0, 300, 60, False, False) is None
    assert mod.next_action(61, 0, 0, 60, 60, True, False) == "poll"   # hb >= poll: off
    assert mod.next_action(90, 0, 0, 120, 300, True, False) is None
