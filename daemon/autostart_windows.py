"""Login-autostart toggle for Clawdmeter — APP-01 / D-07.

Manages a per-user HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run
registry value named "Clawdmeter" that launches the tray app headlessly via
pythonw.exe (no console window — D-08).

winreg is Windows stdlib; this module guards the import so it can be imported
on the Linux dev box (unit tests mock `daemon.autostart_windows.winreg`).

Public API:
  enable(tray_script=None)  -- write/overwrite the Run value
  disable()                 -- remove the Run value; idempotent when absent
  is_enabled()              -- True if the Run value is currently present
"""

import os
import sys
import time

# Guard the import so the module is importable off-Windows.
# Unit tests replace this attribute via:
#   patch("daemon.autostart_windows.winreg", <MagicMock>)
try:
    import winreg as winreg  # type: ignore[import]
except ImportError:
    winreg = None  # type: ignore[assignment]

# Registry key (no leading backslash — OpenKey uses relative path under hive).
_RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
_VALUE_NAME = "Clawdmeter"


def log(msg: str) -> None:
    """Log in the daemon [HH:MM:SS] style."""
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def _command(tray_script: str | None = None) -> str:
    """Build the headless launch command for the Run value.

    Uses the BASE interpreter's pythonw.exe — `sys.base_exec_prefix` points at
    the real Python install even inside a venv — NOT the venv's
    `Scripts\\pythonw.exe`.  The venv pythonw is a redirector stub that
    re-launches the CONSOLE `python.exe` build as a child process (a CPython
    venv-launcher bug, verified empirically on Python 3.13), which pops a black
    console window at logon and kills the tray when closed (field bug, SC#1).
    The base pythonw loads in-process and is genuinely windowless.

    The path is never hard-coded (D-08, CLAUDE.md "repoint ExecStart" lesson);
    both paths are quoted for space safety.  tray_windows.py adds the venv's
    site-packages to sys.path itself, so the venv's deps still resolve under the
    base interpreter.

    Args:
        tray_script: absolute path to the tray entry script.  Defaults to this
                     module's own path (useful when autostart_windows.py IS
                     the entry point, but callers should pass tray_windows.py).
    """
    pythonw = os.path.join(sys.base_exec_prefix, "pythonw.exe")
    script = os.path.abspath(tray_script if tray_script is not None else __file__)
    return f'"{pythonw}" "{script}"'


def enable(tray_script: str | None = None) -> None:
    """Write (or overwrite) the HKCU Run value pointing at pythonw.exe.

    No admin elevation required — HKCU is per-user (D-07, ASVS V4).

    Args:
        tray_script: path to the tray entry script (passed to _command()).
    """
    cmd = _command(tray_script)
    with winreg.OpenKey(
        winreg.HKEY_CURRENT_USER, _RUN_KEY, 0, winreg.KEY_SET_VALUE
    ) as key:
        winreg.SetValueEx(key, _VALUE_NAME, 0, winreg.REG_SZ, cmd)
    log(f"Autostart enabled: {cmd}")


def disable() -> None:
    """Remove the HKCU Run value.  Idempotent — no error if already absent.

    Mirrors the read_token() OSError-swallow pattern (daemon L201-208).
    """
    try:
        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER, _RUN_KEY, 0, winreg.KEY_SET_VALUE
        ) as key:
            winreg.DeleteValue(key, _VALUE_NAME)
        log("Autostart disabled")
    except FileNotFoundError:
        pass  # already absent — idempotent


def is_enabled() -> bool:
    """Return True if the Run value is currently present, False otherwise.

    Queries the live registry on every call so the state reflects external
    changes (e.g. the user deleting the value manually) — Pitfall 6 guard.
    """
    try:
        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER, _RUN_KEY, 0, winreg.KEY_QUERY_VALUE
        ) as key:
            winreg.QueryValueEx(key, _VALUE_NAME)
            return True
    except FileNotFoundError:
        return False
