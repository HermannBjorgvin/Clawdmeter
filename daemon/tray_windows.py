#!/usr/bin/env python3
"""Windows system-tray entry and state bridge for Clawdmeter — APP-01.

Provides:
  TrayState   — thread-safe scalar bridge (daemon loop writes, tray reads)
  header_text — pure helper producing the D-05 status-header string
  main()      — tray entry: builds per-state icons, runs the daemon loop in a
                bg thread, and runs pystray.Icon on the main thread

The daemon loop (claude_usage_daemon_windows.main) is UNCHANGED in logic;
this module injects only additive state-setter calls at existing branch points.

Usage::

    python tray_windows.py

Run: python -m pytest daemon/tests/test_windows_tray.py -x -q
"""

import os
import sys
import threading
import time

# Repo root = the directory that CONTAINS the `daemon` package (this file is
# <repo>/daemon/tray_windows.py). Resolve it from __file__ so the package
# imports below and the brand-logo asset load work no matter what the current
# working directory is — critical for logon autostart, where the HKCU\Run entry
# starts with cwd = System32, not the repo (APP-01 / SC#1).
_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

# Autostart launches us with the BASE interpreter's pythonw.exe, not the venv's
# (see autostart_windows._command — the venv pythonw redirector pops a console
# window). The base interpreter does NOT see the venv's site-packages, so add
# them here to resolve pystray/bleak/PIL. os.path.isdir guards the no-venv and
# already-inside-venv cases; site.addsitedir is a no-op on a missing dir anyway.
_VENV_SITE = os.path.join(_REPO_ROOT, ".venv", "Lib", "site-packages")
if os.path.isdir(_VENV_SITE):
    import site
    site.addsitedir(_VENV_SITE)

# ---------------------------------------------------------------------------
# TrayState — thread-safe scalar bridge (loop -> tray)
# ---------------------------------------------------------------------------

class TrayState:
    """Shared state object bridging the daemon asyncio loop to the tray.

    The daemon loop writes state via the set_* methods; the tray reads the
    scalar attributes.  No lock is needed — writes are atomic attribute
    assignments of simple Python scalars, and the tray only ever reads them.

    The loop populates `loop` and `stop_event` at startup (inside
    daemon_main()) so the tray's Quit handler can route through
    loop.call_soon_threadsafe (RESEARCH Pitfall 2 / Anti-Pattern).
    """

    def __init__(self) -> None:
        self.state: str = "scanning"       # "connected" | "scanning" | "error"
        self.reason: str = ""              # error reason string (D-04)
        self.last_sync: float | None = None  # time.time() of last successful write

        # Populated by daemon main() at startup:
        self.loop = None        # asyncio running loop (for call_soon_threadsafe)
        self.stop_event = None  # asyncio.Event (the existing clean-shutdown hook)

    def set_connected(self, ts: float) -> None:
        """Called after write_payload returns True.  ts = time.time()."""
        self.state = "connected"
        self.reason = ""
        self.last_sync = ts

    def set_scanning(self) -> None:
        """Called in scan/reconnect branches.  BLE churn stays Scanning (D-01)."""
        self.state = "scanning"
        self.reason = ""

    def set_error(self, why: str) -> None:
        """Called on token-expired / API auth failure (D-01 Error = actionable only)."""
        self.state = "error"
        self.reason = why


# ---------------------------------------------------------------------------
# header_text — pure D-05 status header string
# ---------------------------------------------------------------------------

def header_text(ts: TrayState) -> str:
    """Return the D-05 menu status-header string for the current TrayState.

    Shapes:
      "Connected · last update HH:MM"  (ts.last_sync is a float)
      "Connected · last update never"  (ts.last_sync is None)
      "Scanning…"
      "Error: {reason}"
    """
    if ts.state == "connected":
        if ts.last_sync is not None:
            when = time.strftime("%H:%M", time.localtime(ts.last_sync))
        else:
            when = "never"
        return f"Connected · last update {when}"
    if ts.state == "scanning":
        return "Scanning…"   # "Scanning…"
    return f"Error: {ts.reason}"


# ---------------------------------------------------------------------------
# single-instance guard (named kernel mutex — no stale-lock problem)
# ---------------------------------------------------------------------------

# Per-session mutex name. "Local\\" scopes it to the interactive logon, which is
# exactly the granularity we want: one tray per signed-in user. Both the headless
# autostart (HKCU\Run pythonw) and an ARSO-restored console instance live in the
# same session, so this name catches the duplicate-launch collision that produced
# the "mystery console window fighting the headless tray over BLE" field bug.
_SINGLETON_MUTEX_NAME = "Local\\Clawdmeter-tray-singleton"
_ERROR_ALREADY_EXISTS = 183


def _acquire_single_instance():
    """Acquire the process-wide single-instance lock.

    Returns a truthy handle to keep alive for the process lifetime if this is
    the first/only tray, or None if another Clawdmeter tray already owns the
    lock (the caller must then exit immediately, before touching BLE).

    Uses a named kernel mutex: Windows releases it automatically when the owning
    process dies, so there is no stale-lock cleanup (unlike a pidfile). We never
    CloseHandle it — the handle lives until process exit, which is precisely the
    lock lifetime we want.

    Off-Windows (Linux dev box / unit tests) this is a no-op that always
    succeeds — the tray only ever runs on Windows, and the dev box must stay
    importable for the pure-helper tests.
    """
    if sys.platform != "win32":
        return object()  # no-op sentinel; never blocks off-Windows

    import ctypes
    from ctypes import wintypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateMutexW.restype = wintypes.HANDLE
    kernel32.CreateMutexW.argtypes = [wintypes.LPVOID, wintypes.BOOL, wintypes.LPCWSTR]

    handle = kernel32.CreateMutexW(None, True, _SINGLETON_MUTEX_NAME)
    if not handle:
        # Couldn't create the mutex at all — fail OPEN so a kernel quirk never
        # stops the tray from starting; single-instance is best-effort hardening.
        return object()
    if ctypes.get_last_error() == _ERROR_ALREADY_EXISTS:
        return None  # another instance already holds it
    return handle


# ---------------------------------------------------------------------------
# main() — tray entry (pystray on main thread, daemon loop in bg thread)
# ---------------------------------------------------------------------------

def main() -> None:
    """Tray entry point: build icons, start daemon bg thread, run pystray.

    `import pystray` is intentionally INSIDE this function (not at module top)
    so the module can be imported on a GTK-less Linux dev box for unit tests
    of the pure helpers (TrayState, header_text) without pystray failing.
    """
    # Single-instance guard FIRST — before icons, the daemon thread, or any BLE
    # work. If another tray already owns the session mutex (e.g. ARSO restored a
    # console instance and the headless autostart also fired), exit silently.
    # Under pythonw there is no console to print to, so this is a quiet return.
    _instance_lock = _acquire_single_instance()
    if _instance_lock is None:
        return

    import asyncio as _asyncio
    import pystray
    from pystray import Menu, MenuItem

    import daemon.autostart_windows as autostart
    from daemon.claude_usage_daemon_windows import main as daemon_main, log as daemon_log
    from daemon.icon_assets import load_logo_rgba, build_state_icons

    # Build per-state icons once at startup; swap icon.icon per tick (never recomposite).
    base = load_logo_rgba(os.path.join(_REPO_ROOT, "firmware", "src", "logo.h"))
    images = build_state_icons(base)

    ts = TrayState()
    icon = pystray.Icon("Clawdmeter", images["scanning"], "Clawdmeter")

    # --- background thread: asyncio loop ---
    def _run_daemon() -> None:
        # daemon=True thread: an unhandled exception here would vanish silently
        # and freeze the tray on its last state forever (the field "frozen tray"
        # failure mode). Surface it instead — log the traceback to the rotating
        # file and flip the tray to an actionable error state.
        try:
            _asyncio.run(daemon_main(tray_state=ts))
        except Exception as e:  # last-resort thread guard
            import traceback
            daemon_log(f"Daemon thread crashed: {e!r}")
            daemon_log(traceback.format_exc())
            ts.set_error(f"daemon crashed: {type(e).__name__}")

    daemon_thread = threading.Thread(target=_run_daemon, daemon=True)
    daemon_thread.start()

    # --- menu ---
    def _on_quit(icon_ref, _item) -> None:
        # NEVER call ts.stop_event.set() directly from the tray thread;
        # asyncio.Event is NOT thread-safe (RESEARCH Pitfall 2).
        #
        # After signalling, WAIT for the daemon thread to finish its graceful
        # shutdown (the loop's finally: client.disconnect()) BEFORE we stop the
        # icon and let the process exit. Without this join the daemon=True thread
        # is killed mid-flight, the peer never gets a clean GATT disconnect, and
        # the device sits frozen on stale data instead of returning to its waiting
        # screen (SC#3 field report). The timeout caps the block so Quit can never
        # hang if a WinRT disconnect wedges (rare) — we exit anyway as a fallback.
        if ts.loop is not None and ts.stop_event is not None:
            ts.loop.call_soon_threadsafe(ts.stop_event.set)
            daemon_thread.join(timeout=6.0)
        icon_ref.stop()

    def _on_toggle(_icon_ref, _item) -> None:
        if autostart.is_enabled():
            autostart.disable()
        else:
            # Pass THIS file explicitly — without it enable() defaults the Run
            # value to autostart_windows.py (which has no entry point and starts
            # nothing), silently breaking menu-enabled autostart.
            autostart.enable(tray_script=os.path.abspath(__file__))
        icon.update_menu()

    icon.menu = Menu(
        # Non-clickable status header; text updates via update_menu() on state change.
        MenuItem(lambda _item: header_text(ts), None, enabled=False),
        # Start-at-login toggle: checked= is a CALLABLE for live query (Pitfall 6).
        MenuItem("Start at login", _on_toggle, checked=lambda _item: autostart.is_enabled()),
        MenuItem("Quit", _on_quit),
    )

    # --- setup callback (runs in pystray's setup thread, 1s poll) ---
    prev_state: dict = {"state": None, "last_sync": None}

    def _refresh(_icon: pystray.Icon) -> None:
        _icon.visible = True
        while _icon._running:  # type: ignore[attr-defined]
            current = ts.state
            last_sync = ts.last_sync
            state_changed = current != prev_state["state"]
            # Refresh the tooltip/menu when last_sync advances too — not only on
            # state change. A healthy "connected" daemon polling a flat usage
            # value never changes state, so a transition-only refresh froze the
            # "last update HH:MM" tooltip and read as a dead daemon (SC#2 field
            # report: device + tooltip both looked stuck while polling was fine).
            if state_changed or last_sync != prev_state["last_sync"]:
                if state_changed:
                    _icon.icon = images[current]  # icon image depends on state only
                _icon.title = header_text(ts)
                # D-04: toast ONLY on transition INTO error, not on every error tick.
                if current == "error" and prev_state["state"] != "error":
                    _icon.notify(ts.reason or "Clawdmeter error", "Clawdmeter")
                prev_state["state"] = current
                prev_state["last_sync"] = last_sync
                _icon.update_menu()
            time.sleep(1.0)

    # Blocks the main thread until icon.stop() is called from _on_quit.
    icon.run(setup=_refresh)


if __name__ == "__main__":
    main()
