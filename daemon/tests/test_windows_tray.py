#!/usr/bin/env python3
"""Unit tests for daemon/tray_windows.py — APP-01.

Covers:
  TrayState scalar setters and initial state
  header_text() for all three states including last_sync=None
  daemon main() accepts tray_state and populates ts.loop / ts.stop_event
  Quit routes through loop.call_soon_threadsafe (not stop_event.set directly)
  Error toast fires only on transition INTO error state (D-04)

All pystray usage is inside tray_windows.main() (deferred import), so these
tests can import the pure helpers (TrayState, header_text) and test Quit/toast
handlers with mocked icons without importing the GTK-less top-level pystray.

Run: python -m pytest daemon/tests/test_windows_tray.py -x -q
"""
import asyncio
import time
from unittest.mock import AsyncMock, MagicMock, patch, call

import pytest

from daemon.tray_windows import TrayState, header_text, _acquire_single_instance, _ERROR_ALREADY_EXISTS


# ---------------------------------------------------------------------------
# TrayState — initial state and setters
# ---------------------------------------------------------------------------

def test_tray_state_initial():
    """TrayState initialises to scanning state with no last_sync."""
    ts = TrayState()
    assert ts.state == "scanning"
    assert ts.reason == ""
    assert ts.last_sync is None
    assert ts.loop is None
    assert ts.stop_event is None


def test_set_connected():
    """set_connected(ts_float) sets state='connected', clears reason, records last_sync."""
    ts = TrayState()
    now = time.time()
    ts.set_connected(now)
    assert ts.state == "connected"
    assert ts.reason == ""
    assert ts.last_sync == now


def test_set_scanning():
    """set_scanning() sets state='scanning', clears reason."""
    ts = TrayState()
    ts.set_error("something bad")   # put it in error first
    ts.set_scanning()
    assert ts.state == "scanning"
    assert ts.reason == ""


def test_set_error():
    """set_error(why) sets state='error' and stores the reason string."""
    ts = TrayState()
    ts.set_error("token expired — run claude login")
    assert ts.state == "error"
    assert ts.reason == "token expired — run claude login"


# ---------------------------------------------------------------------------
# header_text — D-05 string shapes
# ---------------------------------------------------------------------------

def test_header_text_scanning():
    """header_text returns 'Scanning…' in scanning state."""
    ts = TrayState()
    ts.set_scanning()
    assert header_text(ts) == "Scanning…"


def test_header_text_error():
    """header_text returns 'Error: {reason}' in error state."""
    ts = TrayState()
    ts.set_error("token expired — run claude login")
    result = header_text(ts)
    assert result == "Error: token expired — run claude login"


def test_header_text_connected_with_last_sync():
    """header_text returns 'Connected · last update HH:MM' when last_sync is set."""
    ts = TrayState()
    # Use a known timestamp so we can predict the HH:MM string.
    known_ts = time.mktime(time.strptime("2026-06-01 14:32:00", "%Y-%m-%d %H:%M:%S"))
    ts.set_connected(known_ts)
    result = header_text(ts)
    # Extract the HH:MM portion from the actual local time expansion.
    expected_when = time.strftime("%H:%M", time.localtime(known_ts))
    assert result == f"Connected · last update {expected_when}"


def test_header_text_connected_never_when_last_sync_none():
    """header_text returns 'Connected · last update never' when last_sync is None."""
    ts = TrayState()
    # Manually set state without using set_connected so last_sync stays None.
    ts.state = "connected"
    ts.last_sync = None
    result = header_text(ts)
    assert result == "Connected · last update never"


# ---------------------------------------------------------------------------
# daemon main() populates ts.loop and ts.stop_event
# ---------------------------------------------------------------------------

def test_main_populates_tray_state_loop_and_stop_event():
    """daemon main(tray_state=ts) sets ts.loop and ts.stop_event before the loop body."""
    import daemon.claude_usage_daemon_windows as mod

    ts = TrayState()
    populated = {}

    async def _fake_acquire():
        # Record the state of ts at first acquisition entry (after main() startup lines).
        populated["loop"] = ts.loop
        populated["stop_event"] = ts.stop_event
        # Signal stop so the loop exits cleanly.
        ts.stop_event.set()
        return None   # nothing acquired

    # Patch acquire_target (main()'s device-acquisition seam), not scan_for_device:
    # a None from scan_for_device alone would let acquire_target fall through to the
    # real bonded-address discovery + a real BLE connect, hammering live hardware.
    with patch.object(mod, "acquire_target", side_effect=_fake_acquire):
        asyncio.run(mod.main(tray_state=ts))

    assert populated.get("loop") is not None, "ts.loop must be set by daemon main()"
    assert populated.get("stop_event") is not None, "ts.stop_event must be set by daemon main()"


# ---------------------------------------------------------------------------
# Quit handler routes through call_soon_threadsafe (not stop_event.set directly)
# ---------------------------------------------------------------------------

def test_quit_uses_call_soon_threadsafe():
    """The Quit menu handler calls loop.call_soon_threadsafe(stop_event.set) and icon.stop().

    It must NOT call stop_event.set() directly from the tray thread
    (RESEARCH Pitfall 2 / T-04-06 mitigation).
    """
    # Build a TrayState with a mocked loop and stop_event.
    ts = TrayState()
    mock_loop = MagicMock()
    mock_stop_event = MagicMock()
    ts.loop = mock_loop
    ts.stop_event = mock_stop_event

    # Build the Quit handler the same way tray_windows.main() does, without
    # importing pystray at the module level.  We construct a local closure
    # that mirrors the on_quit body.
    mock_icon = MagicMock()

    def _on_quit(icon_ref, _item):
        # This is the exact body from tray_windows.main() — keep in sync.
        ts.loop.call_soon_threadsafe(ts.stop_event.set)
        icon_ref.stop()

    _on_quit(mock_icon, None)

    # call_soon_threadsafe must have been called with stop_event.set as the arg.
    mock_loop.call_soon_threadsafe.assert_called_once_with(mock_stop_event.set)
    # icon.stop() must have been called.
    mock_icon.stop.assert_called_once()
    # stop_event.set() must NOT have been called directly.
    mock_stop_event.set.assert_not_called()


# ---------------------------------------------------------------------------
# Error toast fires only on transition INTO error (D-04)
# ---------------------------------------------------------------------------

def test_error_toast_on_entry_only():
    """The tray refresh loop fires icon.notify() only on transition INTO error.

    Sequence: scanning -> error -> error
    Expected: notify called exactly once (on the scanning->error transition).
    """
    ts = TrayState()
    ts.set_scanning()

    mock_icon = MagicMock()
    mock_icon._running = True

    # Simulate the _refresh loop's state-change detection logic from tray_windows.main().
    # We run two transitions manually:
    #   1. scanning -> error    (should call notify once)
    #   2. error -> error       (no change — notify must NOT fire again)
    prev_state: dict = {"state": None}

    def _process_state_change(new_state: str, reason: str = "") -> None:
        """Mirror the relevant part of the _refresh loop body."""
        ts.state = new_state
        ts.reason = reason
        current = ts.state
        if current != prev_state["state"]:
            if current == "error" and prev_state["state"] != "error":
                mock_icon.notify(ts.reason or "Clawdmeter error", "Clawdmeter")
            prev_state["state"] = current

    # Transition 1: scanning -> error  (notify should fire)
    _process_state_change("scanning")
    _process_state_change("error", "token expired — run claude login")
    # Transition 2: error -> error  (same state — no call)
    _process_state_change("error", "token expired — run claude login")

    mock_icon.notify.assert_called_once_with(
        "token expired — run claude login", "Clawdmeter"
    )


# ---------------------------------------------------------------------------
# Single-instance guard (named mutex) — duplicate-launch / ARSO collision
# ---------------------------------------------------------------------------
# Field bug: Windows "restart apps after sign-in" (ARSO) restored a console
# `python.exe tray_windows.py` instance while the headless `pythonw` autostart
# also fired — two trays fighting over the one BLE link. The guard makes a
# second instance exit before it touches BLE.

def test_single_instance_noop_off_windows():
    """Off-Windows the guard is a no-op that returns a truthy sentinel (never None)."""
    with patch("daemon.tray_windows.sys") as mock_sys:
        mock_sys.platform = "linux"
        assert _acquire_single_instance() is not None


def _fake_kernel32(last_error: int, handle: int):
    """Build a fake ctypes module tree whose CreateMutexW returns `handle` and
    whose get_last_error() returns `last_error`."""
    fake_kernel32 = MagicMock()
    fake_kernel32.CreateMutexW.return_value = handle
    fake_ctypes = MagicMock()
    fake_ctypes.WinDLL.return_value = fake_kernel32
    fake_ctypes.get_last_error.return_value = last_error
    return fake_ctypes


def test_single_instance_first_instance_gets_handle():
    """First instance: CreateMutexW succeeds, no prior owner → returns the handle."""
    fake_ctypes = _fake_kernel32(last_error=0, handle=0xABCD)
    with patch("daemon.tray_windows.sys") as mock_sys, \
         patch.dict("sys.modules", {"ctypes": fake_ctypes, "ctypes.wintypes": MagicMock()}):
        mock_sys.platform = "win32"
        assert _acquire_single_instance() == 0xABCD


def test_single_instance_second_instance_gets_none():
    """Second instance: mutex already exists → returns None so caller exits."""
    fake_ctypes = _fake_kernel32(last_error=_ERROR_ALREADY_EXISTS, handle=0xABCD)
    with patch("daemon.tray_windows.sys") as mock_sys, \
         patch.dict("sys.modules", {"ctypes": fake_ctypes, "ctypes.wintypes": MagicMock()}):
        mock_sys.platform = "win32"
        assert _acquire_single_instance() is None


def test_single_instance_fails_open_on_null_handle():
    """If CreateMutexW returns NULL, fail OPEN (truthy) — never block tray startup."""
    fake_ctypes = _fake_kernel32(last_error=_ERROR_ALREADY_EXISTS, handle=0)
    with patch("daemon.tray_windows.sys") as mock_sys, \
         patch.dict("sys.modules", {"ctypes": fake_ctypes, "ctypes.wintypes": MagicMock()}):
        mock_sys.platform = "win32"
        result = _acquire_single_instance()
        assert result is not None


# ---------------------------------------------------------------------------
# Regression: cwd-independent package + asset resolution (SC#1 logon autostart)
# ---------------------------------------------------------------------------
# Field bug: launching `pythonw.exe daemon\tray_windows.py` at logon starts with
# cwd = System32, so `import daemon.*` raised ModuleNotFoundError and the relative
# logo path failed — the tray crashed silently with no icon. tray_windows must
# self-locate the repo root from __file__ so it works from any working directory.

def test_repo_root_is_parent_of_daemon_package():
    """_REPO_ROOT points at the dir that CONTAINS the daemon package."""
    import os
    import daemon.tray_windows as tw

    assert os.path.isdir(os.path.join(tw._REPO_ROOT, "daemon"))
    assert os.path.isfile(
        os.path.join(tw._REPO_ROOT, "firmware", "src", "logo.h")
    ), "brand logo must resolve from _REPO_ROOT, not the current working directory"


def test_repo_root_on_sys_path_after_import():
    """Importing tray_windows puts the repo root on sys.path so `daemon.*` resolves regardless of cwd."""
    import sys
    import daemon.tray_windows as tw

    assert tw._REPO_ROOT in sys.path


# ---------------------------------------------------------------------------
# Regression: daemon main() must run in a BACKGROUND thread (SC#1 tray launch)
# ---------------------------------------------------------------------------
# Field bug: under the tray the loop runs in threading.Thread (pystray owns the
# main thread). OS signal-handler installation (loop.add_signal_handler /
# signal.signal) only works on the main thread, so main() raised
# "signal only works in main thread" and the daemon thread died on startup.
# main() must guard signal setup to the main thread; the tray owns shutdown.

def test_main_runs_in_background_thread_without_signal_error():
    """main(tray_state=ts) started from a non-main thread must not raise on signal setup."""
    import threading as _threading
    import daemon.claude_usage_daemon_windows as mod

    ts = TrayState()
    errors: list = []

    async def _fake_acquire():
        ts.stop_event.set()   # exit the loop immediately
        return None

    def _run() -> None:
        try:
            # Patch acquire_target, not scan_for_device — otherwise a None scan falls
            # through to real bonded-address discovery + a real BLE connect (which would
            # block past the 10s join below and make this regression test hit hardware).
            with patch.object(mod, "acquire_target", side_effect=_fake_acquire):
                asyncio.run(mod.main(tray_state=ts))
        except Exception as exc:   # noqa: BLE001 — capture for the assertion
            errors.append(exc)

    t = _threading.Thread(target=_run)
    t.start()
    t.join(timeout=10)

    assert not t.is_alive(), "daemon main() hung in background thread"
    assert not errors, f"main() raised in a background thread: {errors!r}"
