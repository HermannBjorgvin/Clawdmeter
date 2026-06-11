#!/usr/bin/env python3
"""Unit tests for daemon/autostart_windows.py — APP-01.

Covers the winreg HKCU\\Run enable/disable/is_enabled login-autostart toggle.
winreg is NOT importable off-Windows; these tests patch it via
patch("daemon.autostart_windows.winreg", ...) so they run on any platform.

Run: python -m pytest daemon/tests/test_windows_autostart.py -x -q
"""
from unittest.mock import MagicMock, patch, call

import pytest


# ---------------------------------------------------------------------------
# Helpers — build a fake winreg module with the attributes autostart_windows
# references.  Using a MagicMock as the module means all attribute accesses
# on it (HKEY_CURRENT_USER, KEY_SET_VALUE, etc.) automatically produce child
# MagicMocks, which is exactly what we want.
# ---------------------------------------------------------------------------

def _make_winreg_mock(*, query_raises=False):
    """Return a configured MagicMock that stands in for the winreg module."""
    winreg = MagicMock()

    # Constants — assign simple sentinel values so equality checks work.
    winreg.HKEY_CURRENT_USER = "HKEY_CURRENT_USER"
    winreg.KEY_SET_VALUE = 0x0002
    winreg.KEY_QUERY_VALUE = 0x0001
    winreg.REG_SZ = 1

    # OpenKey is used as a context manager; return a MagicMock key handle that
    # supports __enter__ / __exit__.
    key_handle = MagicMock()
    key_handle.__enter__ = MagicMock(return_value=key_handle)
    key_handle.__exit__ = MagicMock(return_value=False)
    winreg.OpenKey = MagicMock(return_value=key_handle)

    # QueryValueEx behaviour is configured by the caller.
    if query_raises:
        winreg.QueryValueEx = MagicMock(side_effect=FileNotFoundError("not found"))
    else:
        winreg.QueryValueEx = MagicMock(return_value=("some_command", 1))

    return winreg, key_handle


# ---------------------------------------------------------------------------
# test_enable_writes_run_value
# ---------------------------------------------------------------------------

def test_enable_writes_run_value():
    """enable() opens HKCU Run key with KEY_SET_VALUE and calls SetValueEx with
    value name 'Clawdmeter' and type REG_SZ."""
    winreg, key_handle = _make_winreg_mock()

    with patch("daemon.autostart_windows.winreg", winreg):
        import daemon.autostart_windows as mod
        mod.enable()

    # OpenKey must have been called with HKCU and the Run key path
    winreg.OpenKey.assert_called_once_with(
        winreg.HKEY_CURRENT_USER,
        r"Software\Microsoft\Windows\CurrentVersion\Run",
        0,
        winreg.KEY_SET_VALUE,
    )

    # SetValueEx must have been called with the correct value name and type
    winreg.SetValueEx.assert_called_once()
    args = winreg.SetValueEx.call_args[0]
    assert args[0] is key_handle, "SetValueEx first arg must be the opened key handle"
    assert args[1] == "Clawdmeter", "Value name must be 'Clawdmeter'"
    assert args[3] == winreg.REG_SZ, "Value type must be REG_SZ"


# ---------------------------------------------------------------------------
# test_command_uses_pythonw
# ---------------------------------------------------------------------------

def test_command_uses_pythonw():
    """The command string written by enable() contains 'pythonw.exe', does NOT
    contain a bare 'python.exe' token (D-08, no console), and is quoted (starts
    with a double-quote character)."""
    winreg, key_handle = _make_winreg_mock()

    captured_commands = []

    def capture_set_value_ex(key, name, reserved, reg_type, value):
        captured_commands.append(value)

    winreg.SetValueEx = MagicMock(side_effect=capture_set_value_ex)

    with patch("daemon.autostart_windows.winreg", winreg):
        import daemon.autostart_windows as mod
        mod.enable()

    assert len(captured_commands) == 1, "SetValueEx must have been called exactly once"
    cmd = captured_commands[0]

    # Must reference pythonw.exe (D-08)
    assert "pythonw.exe" in cmd, f"Command must contain 'pythonw.exe'; got: {cmd!r}"

    # Must NOT contain a bare 'python.exe' (without the 'w') as a standalone token
    # A command like '"...pythonw.exe" ...' is fine; '"...python.exe" ...' is not.
    import re
    assert not re.search(r'(?<![a-z])python\.exe', cmd), (
        f"Command must not reference a bare 'python.exe'; got: {cmd!r}"
    )

    # Must start with a double-quote (paths are quoted for space safety)
    assert cmd.startswith('"'), (
        f"Command must start with '\"' (quoted path); got: {cmd!r}"
    )


# ---------------------------------------------------------------------------
# test_disable_idempotent
# ---------------------------------------------------------------------------

def test_disable_idempotent():
    """disable() calls DeleteValue; when DeleteValue raises FileNotFoundError,
    disable() swallows it and returns without raising (idempotent-on-missing)."""
    winreg, key_handle = _make_winreg_mock()
    winreg.DeleteValue = MagicMock(side_effect=FileNotFoundError("not found"))

    with patch("daemon.autostart_windows.winreg", winreg):
        import daemon.autostart_windows as mod
        # Must not raise even though DeleteValue raises FileNotFoundError
        mod.disable()  # no exception expected

    winreg.DeleteValue.assert_called_once()


def test_disable_calls_delete_value_with_correct_name():
    """disable() calls DeleteValue with the value name 'Clawdmeter'."""
    winreg, key_handle = _make_winreg_mock()
    winreg.DeleteValue = MagicMock()

    with patch("daemon.autostart_windows.winreg", winreg):
        import daemon.autostart_windows as mod
        mod.disable()

    winreg.DeleteValue.assert_called_once()
    args = winreg.DeleteValue.call_args[0]
    assert args[1] == "Clawdmeter", f"DeleteValue must target 'Clawdmeter'; got {args[1]!r}"


# ---------------------------------------------------------------------------
# test_is_enabled
# ---------------------------------------------------------------------------

def test_is_enabled_true_when_value_present():
    """is_enabled() returns True when QueryValueEx succeeds (value is present)."""
    winreg, key_handle = _make_winreg_mock(query_raises=False)

    with patch("daemon.autostart_windows.winreg", winreg):
        import daemon.autostart_windows as mod
        result = mod.is_enabled()

    assert result is True, "is_enabled() must return True when QueryValueEx succeeds"


def test_is_enabled_false_when_value_absent():
    """is_enabled() returns False when QueryValueEx raises FileNotFoundError."""
    winreg, key_handle = _make_winreg_mock(query_raises=True)

    with patch("daemon.autostart_windows.winreg", winreg):
        import daemon.autostart_windows as mod
        result = mod.is_enabled()

    assert result is False, "is_enabled() must return False when QueryValueEx raises FileNotFoundError"
