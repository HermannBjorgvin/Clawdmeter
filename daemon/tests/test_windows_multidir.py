#!/usr/bin/env python3
"""Unit tests for the Windows daemon's multi config-dir active-plan support (#97).

Covers read_config_dirs, read_token_for, PlanSelector, and poll_active_payload,
mirroring daemon/tests/test_macos_multidir.py from #95 — plus the Windows-only
tray-toast semantics (SC#5) that the macOS daemon does not have.

Run: python -m pytest daemon/tests/test_windows_multidir.py -x -q
"""
import asyncio
import json
from pathlib import Path
from unittest.mock import AsyncMock, MagicMock, patch

import daemon.claude_usage_daemon_windows as mod
from daemon.claude_usage_daemon_windows import (
    AuthError,
    PlanSelector,
    read_config_dirs,
    read_token_for,
)


def _run(coro):
    """Run a coroutine on a fresh private loop (order-independent in the full
    suite — see test_windows_poll.py for why get_event_loop() is unsafe here)."""
    loop = asyncio.new_event_loop()
    try:
        return loop.run_until_complete(coro)
    finally:
        loop.close()


# ---------------------------------------------------------------------------
# read_config_dirs — unset MUST mean "keep legacy behavior" ([] not a default
# dir: the Windows default is read_token()'s candidate LIST, #97)
# ---------------------------------------------------------------------------

def test_config_dirs_empty_when_config_file_absent(tmp_path, monkeypatch):
    monkeypatch.setattr(mod, "CONFIG_FILE", tmp_path / "config")  # absent
    assert read_config_dirs() == []


def test_config_dirs_empty_when_key_absent(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("clock = auto\nchime = on\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    assert read_config_dirs() == []


def test_config_dirs_parses_comma_list_and_expands_tilde(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("config_dirs = ~/.claude, ~/.claude-work  # two plans\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    assert read_config_dirs() == [Path.home() / ".claude", Path.home() / ".claude-work"]


def test_config_dirs_blank_value_is_empty(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("config_dirs =\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    assert read_config_dirs() == []


def test_config_dirs_only_commas_is_empty(tmp_path, monkeypatch):
    """A malformed value of separators only must fall back to legacy behavior."""
    cfg = tmp_path / "config"
    cfg.write_text("config_dirs = , ,\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    assert read_config_dirs() == []


def test_config_dirs_drops_empty_entries(tmp_path, monkeypatch):
    cfg = tmp_path / "config"
    cfg.write_text("config_dirs = ~/.claude, , ~/.claude-work,\n")
    monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
    assert read_config_dirs() == [Path.home() / ".claude", Path.home() / ".claude-work"]


# ---------------------------------------------------------------------------
# read_token_for
# ---------------------------------------------------------------------------

def test_token_for_reads_dir_credentials_file(tmp_path):
    (tmp_path / ".credentials.json").write_text(
        json.dumps({"claudeAiOauth": {"accessToken": "sk-ant-test-TOK-X"}})
    )
    assert read_token_for(tmp_path) == "sk-ant-test-TOK-X"


def test_token_for_missing_file_returns_none(tmp_path):
    assert read_token_for(tmp_path / "nonexistent") is None


def test_token_for_empty_token_returns_none(tmp_path):
    """CR-01 parity: an empty accessToken must not be accepted per-dir either."""
    (tmp_path / ".credentials.json").write_text('{"accessToken": ""}')
    assert read_token_for(tmp_path) is None


# ---------------------------------------------------------------------------
# PlanSelector — the "active = recent API activity" rule (#95)
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
# poll_active_payload — unset config_dirs preserves the old single-token path
# ---------------------------------------------------------------------------

def test_unset_routes_through_legacy_read_token(monkeypatch):
    """[] from read_config_dirs -> exactly the old read_token()/poll_api path."""
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [])
    with patch.object(mod, "read_token", return_value="tok") as rt, \
         patch.object(mod, "poll_api", new=AsyncMock(return_value={"s": 7, "ok": True})):
        payload = _run(mod.poll_active_payload(PlanSelector()))
    assert payload == {"s": 7, "ok": True}
    rt.assert_called_once()


def test_unset_no_token_logs_and_toasts(monkeypatch, capsys):
    """Old behavior kept verbatim: missing token logs and fires the toast."""
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [])
    tray_state = MagicMock()
    with patch.object(mod, "read_token", return_value=None):
        assert _run(mod.poll_active_payload(PlanSelector(), tray_state)) is None
    assert "No token; skipping poll" in capsys.readouterr().out
    tray_state.set_error.assert_called_once_with("token expired — run claude login")


def test_unset_autherror_toasts(monkeypatch):
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [])
    tray_state = MagicMock()
    with patch.object(mod, "read_token", return_value="tok"), \
         patch.object(mod, "poll_api", new=AsyncMock(side_effect=AuthError(401))):
        assert _run(mod.poll_active_payload(PlanSelector(), tray_state)) is None
    tray_state.set_error.assert_called_once_with("token expired — run claude login")


def test_unset_transient_failure_does_not_toast(monkeypatch):
    """SC#5 parity: poll_api None (network blip) must NOT fire the toast."""
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [])
    tray_state = MagicMock()
    with patch.object(mod, "read_token", return_value="tok"), \
         patch.object(mod, "poll_api", new=AsyncMock(return_value=None)):
        assert _run(mod.poll_active_payload(PlanSelector(), tray_state)) is None
    tray_state.set_error.assert_not_called()


# ---------------------------------------------------------------------------
# poll_active_payload — multi-dir discovery + active-plan selection
# ---------------------------------------------------------------------------

def test_poll_active_payload_picks_active_and_skips_tokenless(monkeypatch, capsys):
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [A, B])
    monkeypatch.setattr(mod, "read_token_for", lambda d: {A: "tA", B: None}[d])  # B has no token

    async def fake_poll(token):
        return {"s": 25, "ok": True} if token == "tA" else None

    with patch.object(mod, "poll_api", new=AsyncMock(side_effect=fake_poll)):
        payload = _run(mod.poll_active_payload(PlanSelector()))
    assert payload == {"s": 25, "ok": True}  # only A had a token
    assert f"No token in {B}; skipping" in capsys.readouterr().out


def test_poll_active_payload_returns_none_when_all_fail(monkeypatch, capsys):
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [A, B])
    monkeypatch.setattr(mod, "read_token_for", lambda d: None)
    with patch.object(mod, "poll_api", new=AsyncMock(return_value=None)):
        assert _run(mod.poll_active_payload(PlanSelector())) is None
    assert "No usable config dir this cycle" in capsys.readouterr().out


def test_poll_active_payload_selects_higher_util_plan(monkeypatch):
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [A, B])
    monkeypatch.setattr(mod, "read_token_for", lambda d: {A: "tA", B: "tB"}[d])

    async def fake_poll(token):
        return {"s": 12, "ok": True} if token == "tA" else {"s": 40, "ok": True}

    with patch.object(mod, "poll_api", new=AsyncMock(side_effect=fake_poll)):
        payload = _run(mod.poll_active_payload(PlanSelector()))
    assert payload["s"] == 40  # startup -> highest util plan (B)


def test_poll_active_payload_sticky_across_cycles(monkeypatch):
    """The selector state persists across poll cycles: once A shows activity it
    stays active even though B's utilization is higher."""
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [A, B])
    monkeypatch.setattr(mod, "read_token_for", lambda d: {A: "tA", B: "tB"}[d])
    sel = PlanSelector()
    cycles = iter([
        ({A: 10, B: 30}),  # startup -> B
        ({A: 20, B: 30}),  # A rose -> A
        ({A: 20, B: 30}),  # nothing moved -> still A
    ])

    def make_poll(s_by_dir):
        async def fake_poll(token):
            d = A if token == "tA" else B
            return {"s": s_by_dir[d], "ok": True}
        return fake_poll

    for expected_active, s_by_dir in zip([B, A, A], cycles):
        with patch.object(mod, "poll_api", new=AsyncMock(side_effect=make_poll(s_by_dir))):
            payload = _run(mod.poll_active_payload(sel))
        assert payload["s"] == s_by_dir[expected_active]


# ---------------------------------------------------------------------------
# poll_active_payload — Windows tray-toast semantics in multi-dir mode (SC#5)
# ---------------------------------------------------------------------------

def test_multi_all_transient_does_not_toast(monkeypatch):
    """All dirs failing TRANSIENTLY (poll_api None) must not fire the toast."""
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [A, B])
    monkeypatch.setattr(mod, "read_token_for", lambda d: "tok")
    tray_state = MagicMock()
    with patch.object(mod, "poll_api", new=AsyncMock(return_value=None)):
        assert _run(mod.poll_active_payload(PlanSelector(), tray_state)) is None
    tray_state.set_error.assert_not_called()


def test_multi_all_auth_rejected_toasts(monkeypatch):
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [A, B])
    monkeypatch.setattr(mod, "read_token_for", lambda d: "tok")
    tray_state = MagicMock()
    with patch.object(mod, "poll_api", new=AsyncMock(side_effect=AuthError(401))):
        assert _run(mod.poll_active_payload(PlanSelector(), tray_state)) is None
    tray_state.set_error.assert_called_once_with("token expired — run claude login")


def test_multi_no_tokens_anywhere_toasts(monkeypatch):
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [A, B])
    monkeypatch.setattr(mod, "read_token_for", lambda d: None)
    tray_state = MagicMock()
    assert _run(mod.poll_active_payload(PlanSelector(), tray_state)) is None
    tray_state.set_error.assert_called_once_with("token expired — run claude login")


def test_multi_one_expired_one_working_no_toast(monkeypatch):
    """A working plan must not be hidden behind a sibling's expired token: the
    good dir's payload is returned and no 'token expired' toast fires."""
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [A, B])
    monkeypatch.setattr(mod, "read_token_for", lambda d: {A: "tA", B: "tB"}[d])
    tray_state = MagicMock()

    async def fake_poll(token):
        if token == "tA":
            raise AuthError(401)
        return {"s": 33, "ok": True}

    with patch.object(mod, "poll_api", new=AsyncMock(side_effect=fake_poll)):
        payload = _run(mod.poll_active_payload(PlanSelector(), tray_state))
    assert payload == {"s": 33, "ok": True}
    tray_state.set_error.assert_not_called()
