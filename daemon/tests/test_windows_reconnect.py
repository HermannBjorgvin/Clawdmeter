#!/usr/bin/env python3
"""Unit tests for connect_and_run reconnect hardening — BLE-03.

Covers:
  D-01: connect-retry wrapper (post-wake WinRT failure modes)
  D-03: zombie-link consecutive-failure break (stale is_connected)

Run: python -m pytest daemon/tests/test_windows_reconnect.py -x -q
"""
import asyncio
from pathlib import Path
from unittest.mock import AsyncMock, MagicMock, patch

import pytest
from bleak.exc import BleakError

from daemon.claude_usage_daemon_windows import (
    AuthError,
    Session,
    _wait_first,
    connect_and_run,
)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _run(coro):
    """Run a coroutine synchronously for synchronous test functions."""
    return asyncio.run(coro)


def _make_device(address="AA:BB:CC:DD:EE:FF"):
    """Build a minimal fake BLEDevice."""
    device = MagicMock()
    device.address = address
    return device


async def _make_event(set_):
    ev = asyncio.Event()
    if set_:
        ev.set()
    return ev


# ---------------------------------------------------------------------------
# D-01: connect-retry wrapper tests
# ---------------------------------------------------------------------------

def test_connect_retry_exhaustion_on_bleak_error(monkeypatch, capsys):
    """BleakError on every connect attempt exhausts CONNECT_RETRIES then returns False."""
    import daemon.claude_usage_daemon_windows as mod

    device = _make_device()
    stop_event = asyncio.run(_make_event(False))

    mock_client = AsyncMock()
    mock_client.connect = AsyncMock(side_effect=BleakError("Unreachable"))
    mock_client.is_connected = False
    mock_client.disconnect = AsyncMock()

    with patch("daemon.claude_usage_daemon_windows.BleakClient", return_value=mock_client), \
         patch("daemon.claude_usage_daemon_windows.asyncio.sleep", new=AsyncMock()):
        result = _run(connect_and_run(device, stop_event))

    assert result is False
    assert mock_client.connect.call_count == mod.CONNECT_RETRIES


def test_connect_retry_exhaustion_on_timeout_error(monkeypatch, capsys):
    """asyncio.TimeoutError on every connect attempt is treated same as BleakError."""
    import daemon.claude_usage_daemon_windows as mod

    device = _make_device()
    stop_event = asyncio.run(_make_event(False))

    mock_client = AsyncMock()
    mock_client.connect = AsyncMock(side_effect=asyncio.TimeoutError())
    mock_client.is_connected = False
    mock_client.disconnect = AsyncMock()

    with patch("daemon.claude_usage_daemon_windows.BleakClient", return_value=mock_client), \
         patch("daemon.claude_usage_daemon_windows.asyncio.sleep", new=AsyncMock()):
        result = _run(connect_and_run(device, stop_event))

    assert result is False
    assert mock_client.connect.call_count == mod.CONNECT_RETRIES


def test_connect_retry_calls_disconnect_between_attempts(monkeypatch):
    """Guarded disconnect() is called between failed connect attempts."""
    import daemon.claude_usage_daemon_windows as mod

    device = _make_device()
    stop_event = asyncio.run(_make_event(False))

    mock_client = AsyncMock()
    mock_client.connect = AsyncMock(side_effect=BleakError("Unreachable"))
    mock_client.is_connected = False
    mock_client.disconnect = AsyncMock()

    with patch("daemon.claude_usage_daemon_windows.BleakClient", return_value=mock_client), \
         patch("daemon.claude_usage_daemon_windows.asyncio.sleep", new=AsyncMock()):
        _run(connect_and_run(device, stop_event))

    # disconnect is called between attempts (at least CONNECT_RETRIES - 1 times)
    assert mock_client.disconnect.call_count >= mod.CONNECT_RETRIES - 1


def test_connect_success_on_first_attempt_no_extra_retries(monkeypatch):
    """First-attempt success consumes exactly 1 connect call and proceeds past connect block."""
    import daemon.claude_usage_daemon_windows as mod

    device = _make_device()
    # stop_event is set so the loop exits immediately after connecting
    stop_event = asyncio.run(_make_event(True))

    mock_client = AsyncMock()
    mock_client.connect = AsyncMock(return_value=None)  # success
    mock_client.is_connected = True
    mock_client.disconnect = AsyncMock()
    mock_client.start_notify = AsyncMock()
    mock_client.write_gatt_char = AsyncMock(return_value=None)

    with patch("daemon.claude_usage_daemon_windows.BleakClient", return_value=mock_client), \
         patch("daemon.claude_usage_daemon_windows.read_token", return_value="fake-token"), \
         patch("daemon.claude_usage_daemon_windows.poll_api", new=AsyncMock(return_value={"ok": True})):
        _run(connect_and_run(device, stop_event))

    assert mock_client.connect.call_count == 1


def test_connect_retry_exhaustion_does_not_log_token(monkeypatch, capsys):
    """On exhaustion, no log line contains the patched token sentinel (T-03-01)."""
    import daemon.claude_usage_daemon_windows as mod

    TOKEN_SENTINEL = "sk-ant-SUPERSECRET-DO-NOT-LOG-12345"
    device = _make_device()
    stop_event = asyncio.run(_make_event(False))

    mock_client = AsyncMock()
    mock_client.connect = AsyncMock(side_effect=BleakError("Unreachable"))
    mock_client.is_connected = False
    mock_client.disconnect = AsyncMock()

    with patch("daemon.claude_usage_daemon_windows.BleakClient", return_value=mock_client), \
         patch("daemon.claude_usage_daemon_windows.read_token", return_value=TOKEN_SENTINEL), \
         patch("daemon.claude_usage_daemon_windows.asyncio.sleep", new=AsyncMock()):
        _run(connect_and_run(device, stop_event))

    captured = capsys.readouterr()
    assert TOKEN_SENTINEL not in captured.out, "Token sentinel leaked to stdout (T-03-01)"
    assert TOKEN_SENTINEL not in captured.err, "Token sentinel leaked to stderr (T-03-01)"


# ---------------------------------------------------------------------------
# D-03: zombie-link consecutive-failure break tests
# ---------------------------------------------------------------------------

def _make_zombie_client():
    """Build a mock BleakClient that connects successfully but has is_connected stuck True."""
    mock_client = AsyncMock()
    mock_client.connect = AsyncMock(return_value=None)
    mock_client.is_connected = True   # stale flag — never goes False
    mock_client.disconnect = AsyncMock()
    mock_client.start_notify = AsyncMock()
    return mock_client


def test_zombie_link_break_after_limit_consecutive_failures(monkeypatch):
    """Loop breaks after exactly ZOMBIE_BREAK_LIMIT consecutive False writes (default 1)."""
    import daemon.claude_usage_daemon_windows as mod

    device = _make_device()
    stop_event = asyncio.run(_make_event(False))
    mock_client = _make_zombie_client()

    write_call_count = [0]

    async def fake_write_payload(payload):
        write_call_count[0] += 1
        return False  # always fail — zombie link

    fake_session = AsyncMock()
    fake_session.write_payload = fake_write_payload
    fake_session.refresh_requested = MagicMock()
    fake_session.refresh_requested.is_set = MagicMock(return_value=False)
    fake_session.refresh_requested.clear = MagicMock()
    fake_session.refresh_requested.wait = AsyncMock()

    # Force elapsed >= POLL_INTERVAL immediately
    monkeypatch.setattr(mod, "POLL_INTERVAL", 0)

    async def fast_wait_for(coro, timeout):
        raise asyncio.TimeoutError()

    with patch("daemon.claude_usage_daemon_windows.BleakClient", return_value=mock_client), \
         patch("daemon.claude_usage_daemon_windows.Session", return_value=fake_session), \
         patch("daemon.claude_usage_daemon_windows.read_token", return_value="fake-token"), \
         patch("daemon.claude_usage_daemon_windows.poll_api",
               new=AsyncMock(return_value={"ok": True})), \
         patch("daemon.claude_usage_daemon_windows.asyncio.wait_for",
               side_effect=fast_wait_for):
        result = _run(connect_and_run(device, stop_event))

    # With ZOMBIE_BREAK_LIMIT=1, one False write should break the loop
    assert write_call_count[0] == mod.ZOMBIE_BREAK_LIMIT
    # Should return used_successfully=False (no successful write)
    assert result is False


def test_zombie_counter_resets_on_success_with_raised_limit(monkeypatch):
    """A failed write followed by success resets counter (limit raised to 2 to exercise reset)."""
    import daemon.claude_usage_daemon_windows as mod

    device = _make_device()
    stop_event = asyncio.run(_make_event(False))
    mock_client = _make_zombie_client()

    # Sequence: False (counter=1), True (counter reset to 0), False (counter=1 again), break
    write_results = iter([False, True, False])
    write_call_count = [0]

    async def fake_write_payload(payload):
        write_call_count[0] += 1
        try:
            return next(write_results)
        except StopIteration:
            return False

    # After success, subsequent False write breaks at limit=2 (requires 2 consecutive)
    # With limit=2: False (1), True (reset to 0), False (1), False (2 -> break)
    # But we only have 3 items in write_results; after StopIteration returns False.
    # Let's use a longer sequence to ensure reset-then-2-failures trip the break.
    write_results2 = [False, True, False, False]
    write_call_count2 = [0]

    async def fake_write_payload2(payload):
        write_call_count2[0] += 1
        if write_call_count2[0] - 1 < len(write_results2):
            return write_results2[write_call_count2[0] - 1]
        return False

    fake_session = AsyncMock()
    fake_session.write_payload = fake_write_payload2
    fake_session.refresh_requested = MagicMock()
    fake_session.refresh_requested.is_set = MagicMock(return_value=False)
    fake_session.refresh_requested.clear = MagicMock()
    fake_session.refresh_requested.wait = AsyncMock()

    monkeypatch.setattr(mod, "POLL_INTERVAL", 0)
    monkeypatch.setattr(mod, "ZOMBIE_BREAK_LIMIT", 2)  # raise limit to test reset logic

    async def fast_wait_for(coro, timeout):
        raise asyncio.TimeoutError()

    with patch("daemon.claude_usage_daemon_windows.BleakClient", return_value=mock_client), \
         patch("daemon.claude_usage_daemon_windows.Session", return_value=fake_session), \
         patch("daemon.claude_usage_daemon_windows.read_token", return_value="fake-token"), \
         patch("daemon.claude_usage_daemon_windows.poll_api",
               new=AsyncMock(return_value={"ok": True})), \
         patch("daemon.claude_usage_daemon_windows.asyncio.wait_for",
               side_effect=fast_wait_for):
        result = _run(connect_and_run(device, stop_event))

    # With limit=2 and sequence [False, True, False, False]:
    # cycle 1: False -> consecutive_failures=1 (no break, limit=2)
    # cycle 2: True  -> consecutive_failures=0 (reset)
    # cycle 3: False -> consecutive_failures=1 (no break)
    # cycle 4: False -> consecutive_failures=2 -> break
    assert write_call_count2[0] == 4, (
        f"Expected 4 write calls (reset-on-success logic), got {write_call_count2[0]}"
    )
    # used_successfully=True because cycle 2 succeeded
    assert result is True


def test_zombie_break_disconnect_called_in_finally(monkeypatch):
    """The finally block calls client.disconnect() exactly once on the zombie-break path."""
    import daemon.claude_usage_daemon_windows as mod

    device = _make_device()
    stop_event = asyncio.run(_make_event(False))
    mock_client = _make_zombie_client()

    async def fake_write_payload(payload):
        return False  # always fail

    fake_session = AsyncMock()
    fake_session.write_payload = fake_write_payload
    fake_session.refresh_requested = MagicMock()
    fake_session.refresh_requested.is_set = MagicMock(return_value=False)
    fake_session.refresh_requested.clear = MagicMock()
    fake_session.refresh_requested.wait = AsyncMock()

    monkeypatch.setattr(mod, "POLL_INTERVAL", 0)

    async def fast_wait_for(coro, timeout):
        raise asyncio.TimeoutError()

    with patch("daemon.claude_usage_daemon_windows.BleakClient", return_value=mock_client), \
         patch("daemon.claude_usage_daemon_windows.Session", return_value=fake_session), \
         patch("daemon.claude_usage_daemon_windows.read_token", return_value="fake-token"), \
         patch("daemon.claude_usage_daemon_windows.poll_api",
               new=AsyncMock(return_value={"ok": True})), \
         patch("daemon.claude_usage_daemon_windows.asyncio.wait_for",
               side_effect=fast_wait_for):
        _run(connect_and_run(device, stop_event))

    # The finally block calls disconnect() exactly once
    assert mock_client.disconnect.call_count == 1


def test_zombie_break_returns_used_successfully_false(monkeypatch):
    """connect_and_run returns used_successfully=False after zombie break with no writes."""
    import daemon.claude_usage_daemon_windows as mod

    device = _make_device()
    stop_event = asyncio.run(_make_event(False))
    mock_client = _make_zombie_client()

    async def fake_write_payload(payload):
        return False

    fake_session = AsyncMock()
    fake_session.write_payload = fake_write_payload
    fake_session.refresh_requested = MagicMock()
    fake_session.refresh_requested.is_set = MagicMock(return_value=False)
    fake_session.refresh_requested.clear = MagicMock()
    fake_session.refresh_requested.wait = AsyncMock()

    monkeypatch.setattr(mod, "POLL_INTERVAL", 0)

    async def fast_wait_for(coro, timeout):
        raise asyncio.TimeoutError()

    with patch("daemon.claude_usage_daemon_windows.BleakClient", return_value=mock_client), \
         patch("daemon.claude_usage_daemon_windows.Session", return_value=fake_session), \
         patch("daemon.claude_usage_daemon_windows.read_token", return_value="fake-token"), \
         patch("daemon.claude_usage_daemon_windows.poll_api",
               new=AsyncMock(return_value={"ok": True})), \
         patch("daemon.claude_usage_daemon_windows.asyncio.wait_for",
               side_effect=fast_wait_for):
        result = _run(connect_and_run(device, stop_event))

    # main() uses this return value to route into reconnect branch
    assert result is False


# ---------------------------------------------------------------------------
# D-05: split fast-reconnect vs slow-search backoff in main()
# ---------------------------------------------------------------------------

def test_next_backoff_slow_search_doubles_to_60():
    """_next_backoff doubles correctly and never exceeds 60 (slow-search cap)."""
    import daemon.claude_usage_daemon_windows as mod

    values = []
    b = 1
    for _ in range(10):
        b = mod._next_backoff(b, 60)
        values.append(b)

    assert values == [2, 4, 8, 16, 32, 60, 60, 60, 60, 60]
    assert max(values) <= 60


def test_next_backoff_fast_reconnect_doubles_to_cap():
    """_next_backoff doubles correctly and never exceeds RECONNECT_BACKOFF_CAP (default 8)."""
    import daemon.claude_usage_daemon_windows as mod

    cap = mod.RECONNECT_BACKOFF_CAP
    assert cap < 60, "Fast cap must be strictly lower than search cap"

    values = []
    b = 1
    for _ in range(8):
        b = mod._next_backoff(b, cap)
        values.append(b)

    # Should double until hitting the cap, then stay there
    assert max(values) <= cap
    # Should reach the cap (not just stay at 1)
    assert values[-1] == cap


def test_next_backoff_one_to_two():
    """_next_backoff(1, 60) == 2 (basic sanity)."""
    import daemon.claude_usage_daemon_windows as mod

    assert mod._next_backoff(1, 60) == 2


def test_next_backoff_at_cap_stays():
    """_next_backoff(cap, cap) == cap (does not overflow)."""
    import daemon.claude_usage_daemon_windows as mod

    assert mod._next_backoff(mod.RECONNECT_BACKOFF_CAP, mod.RECONNECT_BACKOFF_CAP) == mod.RECONNECT_BACKOFF_CAP


def test_main_scan_miss_uses_search_backoff():
    """When acquire_target returns None, asyncio.wait_for receives search_backoff timeout values."""
    import daemon.claude_usage_daemon_windows as mod

    # Capture main()'s internal stop_event by intercepting asyncio.Event()
    internal_stop_event = [None]
    real_Event = asyncio.Event

    def capturing_Event():
        ev = real_Event()
        internal_stop_event[0] = ev
        return ev

    recorded_timeouts = []
    call_count = [0]
    MAX_CALLS = 3

    async def fake_acquire():
        return None  # nothing acquired (no advert, no bonded addr) -> slow-search regime

    async def fake_wait_for(coro, timeout):
        recorded_timeouts.append(timeout)
        call_count[0] += 1
        if call_count[0] >= MAX_CALLS and internal_stop_event[0] is not None:
            internal_stop_event[0].set()  # terminate main()'s outer while loop
        raise asyncio.TimeoutError()

    with patch("daemon.claude_usage_daemon_windows.asyncio.Event", side_effect=capturing_Event), \
         patch("daemon.claude_usage_daemon_windows.acquire_target", side_effect=fake_acquire), \
         patch("daemon.claude_usage_daemon_windows.asyncio.wait_for", side_effect=fake_wait_for):
        _run(mod.main())

    # Should have recorded timeouts from search_backoff sequence: 1, 2, 4 (then stop)
    assert len(recorded_timeouts) >= 2
    # Timeouts should be doubling (search_backoff sequence)
    assert recorded_timeouts[0] == 1
    assert recorded_timeouts[1] == 2
    # None should exceed the search cap (60)
    assert all(t <= 60 for t in recorded_timeouts)


def test_main_connect_fail_uses_reconnect_backoff():
    """When connect_and_run returns False, asyncio.wait_for receives reconnect_backoff timeouts (fast cap)."""
    import daemon.claude_usage_daemon_windows as mod

    # Capture main()'s internal stop_event
    internal_stop_event = [None]
    real_Event = asyncio.Event

    def capturing_Event():
        ev = real_Event()
        internal_stop_event[0] = ev
        return ev

    fake_device = _make_device()
    recorded_timeouts = []
    call_count = [0]
    MAX_CALLS = 3

    async def fake_scan():
        return fake_device  # always finds device

    async def fake_connect_and_run(device, event, tray_state=None):
        return False  # always fails -> fast-reconnect regime

    async def fake_wait_for(coro, timeout):
        recorded_timeouts.append(timeout)
        call_count[0] += 1
        if call_count[0] >= MAX_CALLS and internal_stop_event[0] is not None:
            internal_stop_event[0].set()
        raise asyncio.TimeoutError()

    with patch("daemon.claude_usage_daemon_windows.asyncio.Event", side_effect=capturing_Event), \
         patch("daemon.claude_usage_daemon_windows.scan_for_device", side_effect=fake_scan), \
         patch("daemon.claude_usage_daemon_windows.connect_and_run", side_effect=fake_connect_and_run), \
         patch("daemon.claude_usage_daemon_windows.asyncio.wait_for", side_effect=fake_wait_for):
        _run(mod.main())

    # Should have recorded timeouts from reconnect_backoff sequence: 1, 2, 4 (then stop)
    assert len(recorded_timeouts) >= 2
    assert recorded_timeouts[0] == 1
    assert recorded_timeouts[1] == 2
    # All timeouts must be at or below RECONNECT_BACKOFF_CAP (fast cap, < 60)
    assert all(t <= mod.RECONNECT_BACKOFF_CAP for t in recorded_timeouts)


def test_main_reconnect_backoff_reset_on_success():
    """A successful connect_and_run (returns True) resets reconnect_backoff to 1."""
    import daemon.claude_usage_daemon_windows as mod

    # Capture main()'s internal stop_event
    internal_stop_event = [None]
    real_Event = asyncio.Event

    def capturing_Event():
        ev = real_Event()
        internal_stop_event[0] = ev
        return ev

    fake_device = _make_device()
    recorded_timeouts = []
    call_count = [0]

    # Sequence: fail (reconnect_backoff=1), succeed (reset), fail (reconnect_backoff=1 again)
    connect_results = [False, True, False]
    connect_idx = [0]

    async def fake_scan():
        return fake_device

    async def fake_connect_and_run(device, event, tray_state=None):
        idx = connect_idx[0]
        connect_idx[0] += 1
        if idx < len(connect_results):
            return connect_results[idx]
        return False

    async def fake_wait_for(coro, timeout):
        recorded_timeouts.append(timeout)
        call_count[0] += 1
        if call_count[0] >= 2 and internal_stop_event[0] is not None:
            internal_stop_event[0].set()  # stop after 2 waits (first fail + post-success fail)
        raise asyncio.TimeoutError()

    with patch("daemon.claude_usage_daemon_windows.asyncio.Event", side_effect=capturing_Event), \
         patch("daemon.claude_usage_daemon_windows.scan_for_device", side_effect=fake_scan), \
         patch("daemon.claude_usage_daemon_windows.connect_and_run", side_effect=fake_connect_and_run), \
         patch("daemon.claude_usage_daemon_windows.asyncio.wait_for", side_effect=fake_wait_for):
        _run(mod.main())

    # First wait: reconnect_backoff=1 (initial failure)
    # Second wait: reconnect_backoff=1 again (reset by success, then another failure)
    assert len(recorded_timeouts) >= 2
    assert recorded_timeouts[0] == 1, f"Expected 1 on first fail, got {recorded_timeouts[0]}"
    assert recorded_timeouts[1] == 1, f"Expected 1 after success reset, got {recorded_timeouts[1]}"


def test_main_no_saved_addr_file_or_skip_addr():
    """main() does not reference SAVED_ADDR_FILE or skip_addr (Windows is stateless - D-04)."""
    import inspect
    import daemon.claude_usage_daemon_windows as mod

    source = inspect.getsource(mod.main)
    assert "SAVED_ADDR_FILE" not in source, "main() must not reference SAVED_ADDR_FILE (D-04)"
    assert "skip_addr" not in source, "main() must not reference skip_addr (macOS-only)"
    assert "retrieve_connected" not in source.lower(), \
        "main() must not reference retrieve_connected (macOS HID path)"


def test_requirements_windows_contains_required_deps():
    """requirements-windows.txt must contain the expected deps.

    Phase 3 (reconnect) added no new deps; Phase 4 (tray) adds pystray + Pillow.
    This test asserts the final expected state: bleak, httpx, pystray, Pillow
    must be present; winreg must NOT be listed (it is stdlib — no install needed).
    """
    req_path = Path(__file__).parent.parent / "requirements-windows.txt"
    content = req_path.read_text()
    lines = {line.strip().lower() for line in content.splitlines()
             if line.strip() and not line.strip().startswith("#")}

    assert "bleak" in lines, "bleak must be in requirements-windows.txt"
    assert "httpx" in lines, "httpx must be in requirements-windows.txt"
    assert "pystray" in lines, "pystray must be in requirements-windows.txt (Phase 4)"
    assert "pillow" in lines, "Pillow must be in requirements-windows.txt (Phase 4)"
    assert "winreg" not in lines, "winreg is stdlib — must NOT be in requirements-windows.txt"


# ---------------------------------------------------------------------------
# G-03-01: start_notify() OSError must not crash the daemon (SC#3 power-cycle)
# ---------------------------------------------------------------------------

def test_start_notify_oserror_does_not_crash_connect_and_run():
    """G-03-01 regression: on post-power-cycle reconnect, WinRT's start_notify()
    CCCD write can raise a raw OSError/WinError when the just-rebooted peer GATT
    server is not yet ready. The optional refresh subscription must degrade
    gracefully — connect_and_run must NOT propagate the OSError and must proceed
    into the poll loop (returning normally), so the daemon never restarts (SC#3/SC#4).
    """
    device = _make_device()
    # stop_event set so the poll loop exits immediately after subscription setup
    stop_event = asyncio.run(_make_event(True))

    mock_client = AsyncMock()
    mock_client.connect = AsyncMock(return_value=None)  # connect succeeds
    mock_client.is_connected = True
    mock_client.disconnect = AsyncMock()
    # The exact failure observed on hardware (SC#3, 2026-06-02):
    # OSError: [WinError -2147023673] The operation was canceled by the user.
    mock_client.start_notify = AsyncMock(
        side_effect=OSError(-2147023673, "The operation was canceled by the user.")
    )

    with patch("daemon.claude_usage_daemon_windows.BleakClient", return_value=mock_client), \
         patch("daemon.claude_usage_daemon_windows.read_token", return_value="fake-token"), \
         patch("daemon.claude_usage_daemon_windows.poll_api", new=AsyncMock(return_value={"ok": True})):
        # Must NOT raise OSError — graceful degradation into the poll loop.
        result = _run(connect_and_run(device, stop_event))

    # start_notify was actually attempted (and raised), but was swallowed.
    assert mock_client.start_notify.call_count == 1
    # Function returned normally instead of propagating the OSError.
    assert result is False
    # The link was cleaned up via the finally block.
    assert mock_client.disconnect.call_count >= 1


# ---------------------------------------------------------------------------
# SC#2 field report: write_payload() OSError must not crash the daemon thread
# ---------------------------------------------------------------------------

def test_write_payload_oserror_returns_false_not_raises():
    """SC#2 regression: write_gatt_char can raise a raw OSError/WinError (NOT a
    BleakError) when the peer GATT server goes transiently unavailable mid-write.
    write_payload must catch it and return False — tripping the zombie-link break
    for a clean reconnect — instead of propagating an uncaught exception that
    silently kills the daemon=True background thread and freezes the tray.
    """
    mock_client = AsyncMock()
    mock_client.write_gatt_char = AsyncMock(
        side_effect=OSError(-2147023673, "The operation was canceled by the user.")
    )
    session = Session(mock_client)

    result = _run(session.write_payload({"ok": True}))

    assert result is False  # caught and reported, not raised
    assert mock_client.write_gatt_char.call_count == 1


def test_write_payload_bleak_error_still_returns_false():
    """The pre-existing BleakError path must keep returning False (no regression
    from widening the except to also cover OSError)."""
    mock_client = AsyncMock()
    mock_client.write_gatt_char = AsyncMock(side_effect=BleakError("disconnected"))
    session = Session(mock_client)

    assert _run(session.write_payload({"ok": True})) is False


# ---------------------------------------------------------------------------
# SC#3 graceful Quit: _wait_first wakes immediately on stop (clean disconnect)
# ---------------------------------------------------------------------------

def test_wait_first_returns_immediately_when_an_event_is_set():
    """The poll loop's TICK wait must break the instant stop_event is set, so the
    finally: client.disconnect() runs before the process exits (SC#3). The outer
    wait_for(2s) fails fast if _wait_first wrongly blocks for the full 30s timeout."""
    async def go():
        refresh = asyncio.Event()
        stop = asyncio.Event()
        stop.set()  # stop signalled
        await asyncio.wait_for(_wait_first(refresh, stop, timeout=30.0), timeout=2.0)
        assert not refresh.is_set()  # loser waiter drained, refresh untouched
    _run(go())


def test_wait_first_returns_after_timeout_when_no_event_set():
    """With neither event set, _wait_first returns after `timeout` (the normal
    poll-tick path) rather than hanging."""
    async def go():
        await asyncio.wait_for(
            _wait_first(asyncio.Event(), asyncio.Event(), timeout=0.05), timeout=2.0
        )
    _run(go())


# ---------------------------------------------------------------------------
# SC#5: transient poll failure must NOT toast "token expired"; only a real
# 401/403 (AuthError) should. A boot-time DNS blip returns None, not AuthError.
# ---------------------------------------------------------------------------

def _connected_mock_client():
    client = AsyncMock()
    client.connect = AsyncMock(return_value=None)
    client.is_connected = True
    client.disconnect = AsyncMock()
    client.start_notify = AsyncMock()
    return client


def test_transient_poll_failure_does_not_set_error():
    """poll_api returning None (network/DNS, timeout, 5xx, 429) is transient and
    must leave the tray state untouched — not flip it to 'token expired' (SC#5
    field report: `getaddrinfo failed` at boot wrongly fired the toast)."""
    device = _make_device()
    stop_event = asyncio.run(_make_event(False))
    tray_state = MagicMock()
    client = _connected_mock_client()

    async def fake_poll(_token):
        stop_event.set()  # end the loop after this single transient failure
        return None

    with patch("daemon.claude_usage_daemon_windows.BleakClient", return_value=client), \
         patch("daemon.claude_usage_daemon_windows.read_token", return_value="tok"), \
         patch("daemon.claude_usage_daemon_windows.poll_api", new=fake_poll):
        _run(connect_and_run(device, stop_event, tray_state))

    tray_state.set_error.assert_not_called()
    tray_state.set_connected.assert_not_called()


def test_auth_error_sets_token_expired():
    """A genuine 401/403 surfaces as AuthError and DOES flip the tray to the
    actionable 'token expired — run claude login' error state."""
    device = _make_device()
    stop_event = asyncio.run(_make_event(False))
    tray_state = MagicMock()
    client = _connected_mock_client()

    async def fake_poll(_token):
        stop_event.set()
        raise AuthError(401)

    with patch("daemon.claude_usage_daemon_windows.BleakClient", return_value=client), \
         patch("daemon.claude_usage_daemon_windows.read_token", return_value="tok"), \
         patch("daemon.claude_usage_daemon_windows.poll_api", new=fake_poll):
        _run(connect_and_run(device, stop_event, tray_state))

    tray_state.set_error.assert_called_once_with("token expired — run claude login")
