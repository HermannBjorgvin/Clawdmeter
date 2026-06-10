#!/usr/bin/env python3
"""Static no-WSL-paths regression guard — APP-02 / D-10.

Asserts that the daemon, tray, and autostart sources reference no WSL-specific
paths.  This is a CI-surviving regression lock that needs no hardware: if a
future edit accidentally introduces a ``\\wsl$``, ``wsl.exe``, ``/home/``, or
``/mnt/`` reference into any of the three core Windows daemon source files, this
test will fail with a message that names the offending pattern and file.

Run: python -m pytest daemon/tests/test_windows_no_wsl.py -x -q
"""
import re
from pathlib import Path

# The four WSL-path patterns that must never appear in the daemon sources.
FORBIDDEN = [r"\\wsl\$", r"wsl\.exe", r"/home/", r"/mnt/"]

# The three Windows-daemon source files covered by the guard.
SOURCES = [
    Path("daemon/claude_usage_daemon_windows.py"),
    Path("daemon/tray_windows.py"),
    Path("daemon/autostart_windows.py"),
]


def test_no_wsl_paths_in_daemon():
    """daemon/claude_usage_daemon_windows.py references no WSL paths."""
    _assert_clean(Path("daemon/claude_usage_daemon_windows.py"))


def test_no_wsl_paths_in_tray():
    """daemon/tray_windows.py references no WSL paths."""
    _assert_clean(Path("daemon/tray_windows.py"))


def test_no_wsl_paths_in_autostart():
    """daemon/autostart_windows.py references no WSL paths."""
    _assert_clean(Path("daemon/autostart_windows.py"))


def _assert_clean(source: Path) -> None:
    """Assert that none of the FORBIDDEN patterns appear in the given source file.

    Reads the file relative to the repository root (the cwd pytest is invoked
    from).  Fails with a descriptive message naming the leaked pattern and file
    so the regression is immediately actionable.
    """
    text = source.read_text(encoding="utf-8")
    for pat in FORBIDDEN:
        match = re.search(pat, text)
        assert match is None, (
            f"WSL path leaked into {source}: pattern {pat!r} found at "
            f"position {match.start()} — "
            f"context: {text[max(0, match.start()-20):match.end()+20]!r}"
        )
