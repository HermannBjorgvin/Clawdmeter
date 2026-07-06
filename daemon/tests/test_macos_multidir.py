#!/usr/bin/env python3
"""Unit tests for the macOS/Linux daemon's multi config-dir active-plan support.

Covers read_config_dirs, read_token_for, PlanSelector, and poll_active_payload.

Run: python -m pytest daemon/tests/test_macos_multidir.py -x -q
"""
import asyncio
from pathlib import Path
from unittest.mock import AsyncMock, patch

import daemon.claude_usage_daemon as mod
from daemon.claude_usage_daemon import PlanSelector, read_config_dirs, read_token_for


def _run(coro):
    return asyncio.run(coro)


# ---------------------------------------------------------------------------
# read_config_dirs
# ---------------------------------------------------------------------------

def test_config_dirs_defaults_to_claude_when_unset(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "CONFIG_FILE", tmp_path / "config")  # absent
    assert read_config_dirs() == [mod.DEFAULT_CONFIG_DIR]


def test_config_dirs_defaults_when_key_absent(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("clock = auto\nchime = on\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    assert read_config_dirs() == [mod.DEFAULT_CONFIG_DIR]


def test_config_dirs_parses_comma_list_and_expands_tilde(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("config_dirs = ~/.claude, ~/.claude-work  # two plans\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    assert read_config_dirs() == [Path.home() / ".claude", Path.home() / ".claude-work"]


# ---------------------------------------------------------------------------
# read_token_for
# ---------------------------------------------------------------------------

def test_token_for_reads_dir_credentials_file(tmp_path):
    (tmp_path / ".credentials.json").write_text('{"claudeAiOauth":{"accessToken":"TOK_X"}}')
    assert read_token_for(tmp_path) == "TOK_X"


def test_token_for_missing_file_non_default_returns_none(tmp_path, monkeypatch):
    monkeypatch.setattr(mod.sys, "platform", "linux")
    assert read_token_for(tmp_path) is None  # no file, not the default dir


def test_token_for_default_dir_falls_back_to_keychain_on_macos(tmp_path, monkeypatch):
    # An empty dir standing in as the default: no file present -> Keychain.
    monkeypatch.setattr(mod, "DEFAULT_CONFIG_DIR", tmp_path)
    monkeypatch.setattr(mod.sys, "platform", "darwin")
    with patch.object(mod, "_read_token_keychain", return_value="TOK_KEYCHAIN"):
        assert read_token_for(tmp_path) == "TOK_KEYCHAIN"


def test_token_for_file_wins_over_keychain(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "DEFAULT_CONFIG_DIR", tmp_path)
    monkeypatch.setattr(mod.sys, "platform", "darwin")
    (tmp_path / ".credentials.json").write_text('{"accessToken":"TOK_FILE"}')
    with patch.object(mod, "_read_token_keychain", return_value="TOK_KEYCHAIN"):
        assert read_token_for(tmp_path) == "TOK_FILE"


# ---------------------------------------------------------------------------
# PlanSelector — the "active = recent API activity" rule
# ---------------------------------------------------------------------------

A, B = Path("/a"), Path("/b")


def test_selector_startup_picks_highest_util():
    sel = PlanSelector()
    assert sel.choose({A: 10, B: 30}) == B  # no history yet -> highest %


def test_selector_switches_on_rise():
    sel = PlanSelector()
    sel.choose({A: 10, B: 30})           # startup -> B
    assert sel.choose({A: 20, B: 30}) == A  # A rose 10->20 -> A active


def test_selector_sticky_when_no_movement():
    sel = PlanSelector()
    sel.choose({A: 10, B: 30})
    sel.choose({A: 20, B: 30})           # A active
    assert sel.choose({A: 20, B: 30}) == A  # nothing moved -> still A (not higher B)


def test_selector_reset_to_zero_is_not_activity():
    sel = PlanSelector()
    sel.choose({A: 10, B: 30})
    sel.choose({A: 20, B: 30})           # A active
    sel.choose({A: 20, B: 45})           # B rose -> B active
    assert sel.choose({A: 20, B: 0}) == B   # B window reset (drop) isn't a rise -> stays B


def test_selector_larger_rise_wins_same_cycle():
    sel = PlanSelector()
    sel.choose({A: 10, B: 10})           # seed
    assert sel.choose({A: 12, B: 40}) == B  # both rose same cycle -> higher % breaks tie


# ---------------------------------------------------------------------------
# poll_active_payload — integration over the helpers
# ---------------------------------------------------------------------------

def test_poll_active_payload_picks_active_and_skips_tokenless(monkeypatch):
    dirs = [A, B]
    monkeypatch.setattr(mod, "read_config_dirs", lambda: dirs)
    monkeypatch.setattr(mod, "read_token_for", lambda d: {A: "tA", B: None}[d])  # B has no token

    async def fake_poll(token):
        return {"s": 25, "ok": True} if token == "tA" else None

    sel = PlanSelector()
    with patch.object(mod, "poll_api", new=AsyncMock(side_effect=fake_poll)):
        payload = _run(mod.poll_active_payload(sel))
    assert payload == {"s": 25, "ok": True}  # only A had a token


def test_poll_active_payload_returns_none_when_all_fail(monkeypatch):
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [A, B])
    monkeypatch.setattr(mod, "read_token_for", lambda d: None)
    with patch.object(mod, "poll_api", new=AsyncMock(return_value=None)):
        assert _run(mod.poll_active_payload(PlanSelector())) is None


def test_poll_active_payload_selects_higher_util_plan(monkeypatch):
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [A, B])
    monkeypatch.setattr(mod, "read_token_for", lambda d: {A: "tA", B: "tB"}[d])

    async def fake_poll(token):
        return {"s": 12, "ok": True} if token == "tA" else {"s": 40, "ok": True}

    with patch.object(mod, "poll_api", new=AsyncMock(side_effect=fake_poll)):
        payload = _run(mod.poll_active_payload(PlanSelector()))
    assert payload["s"] == 40  # startup -> highest util plan (B)


# ---------------------------------------------------------------------------
# discover_target — the daemon only ever targets the device this system already
# holds; it never scans for a nearby device by name (there is no scan fallback).
# ---------------------------------------------------------------------------

def test_discover_target_darwin_uses_os_held_device(monkeypatch):
    monkeypatch.setattr(mod.sys, "platform", "darwin")
    sentinel = object()
    with patch.object(mod, "retrieve_connected_macos", new=AsyncMock(return_value=sentinel)):
        assert _run(mod.discover_target()) is sentinel  # used directly, no scan


def test_discover_target_darwin_returns_none_when_not_held(monkeypatch):
    # Not held by the OS -> wait (return None); never grabs an arbitrary device.
    monkeypatch.setattr(mod.sys, "platform", "darwin")
    with patch.object(mod, "retrieve_connected_macos", new=AsyncMock(return_value=None)):
        assert _run(mod.discover_target()) is None


def test_discover_target_non_darwin_uses_pinned_address(monkeypatch):
    monkeypatch.setattr(mod.sys, "platform", "linux")
    monkeypatch.setattr(mod, "load_cached_address", lambda: "AA:BB:CC:DD:EE:FF")
    assert _run(mod.discover_target()) == "AA:BB:CC:DD:EE:FF"


def test_discover_target_non_darwin_returns_none_without_pin(monkeypatch):
    # No pinned address cached -> wait; never scans by name.
    monkeypatch.setattr(mod.sys, "platform", "linux")
    monkeypatch.setattr(mod, "load_cached_address", lambda: None)
    assert _run(mod.discover_target()) is None
