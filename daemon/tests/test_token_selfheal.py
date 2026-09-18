#!/usr/bin/env python3
"""Tests for the opt-in token self-heal nudge.

When every configured config dir is 401ing, the device sits on "No data" until
something runs Claude Code. With `token_refresh = on` the daemon runs one tiny
headless Claude Code call so the CLI refreshes its own token. The contract:

  - off by default -> never spawns anything
  - on             -> spawns the CLI once, with the cheap model
  - rate-limited   -> a second attempt inside the window is skipped
  - CLI missing    -> logs and gives up, no crash
  - timeout/failure-> never raises into the poll loop
  - live token     -> never fires at all

Both Python daemons are exercised. No real subprocesses are spawned.

Run: python -m pytest daemon/tests/test_token_selfheal.py -x -q
"""
import asyncio
from unittest.mock import AsyncMock, MagicMock, patch

import pytest

from daemon import claude_usage_daemon as mac_daemon
from daemon import claude_usage_daemon_windows as win_daemon

DAEMONS = [
    pytest.param(mac_daemon, id="macos"),
    pytest.param(win_daemon, id="windows"),
]


def _run(coro):
    loop = asyncio.new_event_loop()
    try:
        return loop.run_until_complete(coro)
    finally:
        loop.close()


@pytest.fixture(autouse=True)
def _reset_nudge_clock():
    """Clear the rate-limit stamp so tests don't leak state into each other."""
    for d in (mac_daemon, win_daemon):
        d._last_nudge_ms = None
    yield
    for d in (mac_daemon, win_daemon):
        d._last_nudge_ms = None


def _fake_proc(returncode=0):
    proc = MagicMock()
    proc.returncode = returncode
    proc.wait = AsyncMock(return_value=returncode)
    proc.kill = MagicMock()
    return proc


@pytest.mark.parametrize("daemon", DAEMONS)
def test_disabled_by_default_never_spawns(daemon):
    """No config (or token_refresh=off) must spend none of the user's quota."""
    with patch.object(daemon, "read_token_refresh_setting", return_value="off"), \
         patch.object(daemon, "find_claude_cli", return_value="/fake/claude"), \
         patch("asyncio.create_subprocess_exec") as spawn:
        assert _run(daemon.nudge_token_refresh()) is False
    spawn.assert_not_called()


@pytest.mark.parametrize("daemon", DAEMONS)
def test_enabled_spawns_cheap_model_once(daemon):
    with patch.object(daemon, "read_token_refresh_setting", return_value="on"), \
         patch.object(daemon, "find_claude_cli", return_value="/fake/claude"), \
         patch("asyncio.create_subprocess_exec",
               AsyncMock(return_value=_fake_proc(0))) as spawn:
        assert _run(daemon.nudge_token_refresh()) is True
    spawn.assert_called_once()
    argv = spawn.call_args[0]
    assert argv[0] == "/fake/claude"
    assert "-p" in argv
    assert daemon.NUDGE_MODEL in argv, "must pin the cheapest model"


@pytest.mark.parametrize("daemon", DAEMONS)
def test_rate_limited_within_window(daemon):
    """A second nudge inside NUDGE_MIN_INTERVAL is skipped — a dead refresh
    token must not turn into a spawn-per-poll loop."""
    with patch.object(daemon, "read_token_refresh_setting", return_value="on"), \
         patch.object(daemon, "find_claude_cli", return_value="/fake/claude"), \
         patch("asyncio.create_subprocess_exec",
               AsyncMock(return_value=_fake_proc(0))) as spawn:
        assert _run(daemon.nudge_token_refresh()) is True
        assert _run(daemon.nudge_token_refresh()) is False
    assert spawn.call_count == 1


@pytest.mark.parametrize("daemon", DAEMONS)
def test_rate_limit_stamped_even_when_cli_missing(daemon):
    """A missing CLI must not retry every poll either."""
    with patch.object(daemon, "read_token_refresh_setting", return_value="on"), \
         patch.object(daemon, "find_claude_cli", return_value=None), \
         patch("asyncio.create_subprocess_exec") as spawn:
        assert _run(daemon.nudge_token_refresh()) is False
        assert _run(daemon.nudge_token_refresh()) is False
    spawn.assert_not_called()


@pytest.mark.parametrize("daemon", DAEMONS)
def test_nonzero_exit_is_not_fatal(daemon):
    with patch.object(daemon, "read_token_refresh_setting", return_value="on"), \
         patch.object(daemon, "find_claude_cli", return_value="/fake/claude"), \
         patch("asyncio.create_subprocess_exec",
               AsyncMock(return_value=_fake_proc(1))):
        assert _run(daemon.nudge_token_refresh()) is False


@pytest.mark.parametrize("daemon", DAEMONS)
def test_spawn_oserror_is_not_fatal(daemon):
    with patch.object(daemon, "read_token_refresh_setting", return_value="on"), \
         patch.object(daemon, "find_claude_cli", return_value="/fake/claude"), \
         patch("asyncio.create_subprocess_exec",
               AsyncMock(side_effect=OSError("no such file"))):
        assert _run(daemon.nudge_token_refresh()) is False


@pytest.mark.parametrize("daemon", DAEMONS)
def test_hung_nudge_is_killed_and_returns_false(daemon):
    """The nudge must never wedge the single-threaded poll loop."""
    proc = _fake_proc(0)
    proc.wait = AsyncMock(side_effect=asyncio.TimeoutError())
    with patch.object(daemon, "read_token_refresh_setting", return_value="on"), \
         patch.object(daemon, "find_claude_cli", return_value="/fake/claude"), \
         patch("asyncio.create_subprocess_exec", AsyncMock(return_value=proc)), \
         patch("asyncio.wait_for", AsyncMock(side_effect=asyncio.TimeoutError())):
        assert _run(daemon.nudge_token_refresh()) is False
    proc.kill.assert_called_once()


# ---------------------------------------------------------------------------
# find_claude_cli
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("daemon", DAEMONS)
def test_config_override_wins_and_must_be_executable(daemon, tmp_path):
    cli = tmp_path / "claude"
    cli.write_text("#!/bin/sh\n")
    cli.chmod(0o755)
    with patch.object(daemon, "read_config_value", return_value=str(cli)):
        assert daemon.find_claude_cli() == str(cli)
    missing = tmp_path / "nope"
    with patch.object(daemon, "read_config_value", return_value=str(missing)):
        assert daemon.find_claude_cli() is None


@pytest.mark.parametrize("daemon", DAEMONS)
def test_falls_back_to_path_lookup(daemon):
    with patch.object(daemon, "read_config_value", return_value=""), \
         patch("shutil.which", return_value="/usr/bin/claude"):
        assert daemon.find_claude_cli() == "/usr/bin/claude"


# ---------------------------------------------------------------------------
# Integration: only fires when every dir is dead (macOS poll_active)
# ---------------------------------------------------------------------------

def test_poll_active_nudges_only_when_all_dirs_dead(tmp_path):
    d = mac_daemon
    with patch.object(d, "read_config_dirs", return_value=[tmp_path]), \
         patch.object(d, "read_token_for", return_value="tok"), \
         patch.object(d, "poll_api", AsyncMock(side_effect=d.TokenExpired())), \
         patch.object(d, "nudge_token_refresh", AsyncMock(return_value=True)) as nudge:
        payload, dead = _run(d.poll_active(d.PlanSelector()))
    assert payload is None and dead is True
    nudge.assert_awaited_once()


def test_poll_active_does_not_nudge_on_transient_failure(tmp_path):
    """A live token that simply didn't answer this cycle is NOT an auth problem
    — nudging there would spend quota on a network blip."""
    d = mac_daemon
    with patch.object(d, "read_config_dirs", return_value=[tmp_path]), \
         patch.object(d, "read_token_for", return_value="tok"), \
         patch.object(d, "poll_api", AsyncMock(return_value=None)), \
         patch.object(d, "nudge_token_refresh", AsyncMock(return_value=True)) as nudge:
        payload, dead = _run(d.poll_active(d.PlanSelector()))
    assert payload is None and dead is False
    nudge.assert_not_awaited()


def test_poll_active_does_not_nudge_when_healthy(tmp_path):
    d = mac_daemon
    with patch.object(d, "read_config_dirs", return_value=[tmp_path]), \
         patch.object(d, "read_token_for", return_value="tok"), \
         patch.object(d, "poll_api", AsyncMock(return_value={"s": 5, "ok": True})), \
         patch.object(d, "nudge_token_refresh", AsyncMock(return_value=True)) as nudge:
        payload, dead = _run(d.poll_active(d.PlanSelector()))
    assert payload is not None and dead is False
    nudge.assert_not_awaited()
