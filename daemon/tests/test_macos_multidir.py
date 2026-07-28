#!/usr/bin/env python3
"""Unit tests for the macOS/Linux daemon's multi config-dir active-plan support.

Covers read_config_dirs, token_sources_for, PlanSelector, and
poll_active_payload.

Run: python -m pytest daemon/tests/test_macos_multidir.py -x -q
"""
import asyncio
import time
from pathlib import Path
from unittest.mock import AsyncMock, patch

import daemon.claude_usage_daemon as mod
from daemon.claude_usage_daemon import (
    Credentials, PlanSelector, read_config_dirs, token_sources_for)


def _creds(token, expires_at=None, source="test"):
    return Credentials(token, expires_at, source)


def _tokens(config_dir):
    return [c.token for c in token_sources_for(config_dir)]


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
# token_sources_for — the ordered credential fallback chain
# ---------------------------------------------------------------------------

def test_sources_read_dir_credentials_file(tmp_path):
    (tmp_path / ".credentials.json").write_text('{"claudeAiOauth":{"accessToken":"TOK_X"}}')
    assert _tokens(tmp_path) == ["TOK_X"]


def test_sources_missing_file_non_default_is_empty(tmp_path, monkeypatch):
    monkeypatch.setattr(mod.sys, "platform", "linux")
    assert _tokens(tmp_path) == []  # no file, not the default dir


def test_sources_default_dir_falls_back_to_keychain_on_macos(tmp_path, monkeypatch):
    # An empty dir standing in as the default: no file present -> Keychain.
    monkeypatch.setattr(mod, "DEFAULT_CONFIG_DIR", tmp_path)
    monkeypatch.setattr(mod.sys, "platform", "darwin")
    with patch.object(mod, "_read_keychain",
                      side_effect=lambda s, optional=False:
                          None if optional else _creds("TOK_KEYCHAIN")):
        assert _tokens(tmp_path) == ["TOK_KEYCHAIN"]


def test_sources_dedicated_token_leads_then_file_then_keychain(tmp_path, monkeypatch):
    # The long-lived token wins, but the others stay as fallbacks behind it.
    monkeypatch.setattr(mod, "DEFAULT_CONFIG_DIR", tmp_path)
    monkeypatch.setattr(mod.sys, "platform", "darwin")
    (tmp_path / ".credentials.json").write_text('{"accessToken":"TOK_FILE"}')
    with patch.object(mod, "_read_keychain",
                      side_effect=lambda s, optional=False:
                          _creds("TOK_LONG") if optional else _creds("TOK_KEYCHAIN")):
        assert _tokens(tmp_path) == ["TOK_LONG", "TOK_FILE", "TOK_KEYCHAIN"]


def test_credentials_expiry_is_read_and_honoured(tmp_path):
    now_ms = int((time.time() - 60) * 1000)
    (tmp_path / ".credentials.json").write_text(
        '{"claudeAiOauth":{"accessToken":"TOK_OLD","expiresAt":%d}}' % now_ms)
    creds = token_sources_for(tmp_path)[0]
    assert creds.expired()  # past expiry -> skipped without an API call


def test_credentials_without_expiry_never_expire(tmp_path):
    # A `claude setup-token` credential advertises no expiry.
    (tmp_path / ".credentials.json").write_text('{"accessToken":"TOK_LONG"}')
    assert not token_sources_for(tmp_path)[0].expired()


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
    monkeypatch.setattr(mod, "token_sources_for",
                        lambda d: {A: [_creds("tA")], B: []}[d])  # B has no token

    async def fake_poll(token):
        return {"s": 25, "ok": True} if token == "tA" else "auth"

    sel = PlanSelector()
    with patch.object(mod, "poll_api", new=AsyncMock(side_effect=fake_poll)):
        payload = _run(mod.poll_active_payload(sel))
    assert payload == {"s": 25, "ok": True}  # only A had a token


def test_poll_active_payload_error_beat_when_all_fail(monkeypatch):
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [A, B])
    monkeypatch.setattr(mod, "token_sources_for", lambda d: [])
    with patch.object(mod, "poll_api", new=AsyncMock(return_value="auth")):
        payload = _run(mod.poll_active_payload(PlanSelector()))
    assert payload == {"ok": False, "err": "token"}  # no dir had a token


def test_poll_active_payload_selects_higher_util_plan(monkeypatch):
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [A, B])
    monkeypatch.setattr(mod, "token_sources_for",
                        lambda d: [_creds({A: "tA", B: "tB"}[d])])

    async def fake_poll(token):
        return {"s": 12, "ok": True} if token == "tA" else {"s": 40, "ok": True}

    with patch.object(mod, "poll_api", new=AsyncMock(side_effect=fake_poll)):
        payload = _run(mod.poll_active_payload(PlanSelector()))
    assert payload["s"] == 40  # startup -> highest util plan (B)


def test_poll_falls_back_to_next_source_when_first_is_rejected(monkeypatch):
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [A])
    monkeypatch.setattr(mod, "token_sources_for",
                        lambda d: [_creds("dead", source="dedicated"),
                                   _creds("live", source="keychain")])

    async def fake_poll(token):
        return "auth" if token == "dead" else {"s": 5, "ok": True}

    with patch.object(mod, "poll_api", new=AsyncMock(side_effect=fake_poll)):
        assert _run(mod.poll_active_payload(PlanSelector())) == {"s": 5, "ok": True}


def test_poll_skips_expired_source_without_calling_the_api(monkeypatch):
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [A])
    monkeypatch.setattr(mod, "token_sources_for",
                        lambda d: [_creds("stale", time.time() - 1), _creds("live")])
    seen = []

    async def fake_poll(token):
        seen.append(token)
        return {"s": 7, "ok": True}

    with patch.object(mod, "poll_api", new=AsyncMock(side_effect=fake_poll)):
        assert _run(mod.poll_active_payload(PlanSelector())) == {"s": 7, "ok": True}
    assert seen == ["live"]  # the expired token never reached the API


def test_poll_publishes_fingerprint_of_the_sources_it_read(monkeypatch):
    # The connected loop compares against this instead of re-reading Keychain.
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [A])
    monkeypatch.setattr(mod, "token_sources_for", lambda d: [_creds("tA")])
    with patch.object(mod, "poll_api", new=AsyncMock(return_value={"s": 1, "ok": True})):
        _run(mod.poll_active_payload(PlanSelector()))
    assert mod._last_fingerprint == mod.fingerprint([_creds("tA")])
    assert "tA" not in mod._last_fingerprint  # hashed, never the token itself


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
