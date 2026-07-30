#!/usr/bin/env python3
"""Guard that the WinRT GATT-cache bypass actually reaches bleak -- CACHE-01.

Why this exists: the daemon passed `use_cached_services=False` and
`address_type="random"` as TOP-LEVEL BleakClient kwargs. That worked on bleak
0.2x. bleak 3.x moved them under `winrt=`, and its WinRT backend reads them only
via `winrt.get(...)` -- so the old form was swallowed by `**kwargs` and silently
did nothing, with no warning and no error.

The visible symptom was nasty and misleading: after flashing firmware that added
a GATT characteristic, Windows kept serving its cached attribute table and the
daemon reported the new characteristic as "not found", which reads exactly like a
firmware bug rather than a host caching bug.

These tests assert the options land on the backend object, so the next bleak
upgrade that moves them fails here instead of in the field.

Run: python -m pytest daemon/tests/test_windows_gatt_cache.py -x -q
"""
import inspect
import sys

import pytest

pytestmark = pytest.mark.skipif(
    sys.platform != "win32", reason="WinRT backend is Windows-only"
)


def _winrt_backend_cls():
    from bleak.backends.winrt.client import BleakClientWinRT
    return BleakClientWinRT


def test_backend_reads_options_from_the_winrt_dict():
    """Pins the contract this guard depends on: the backend sources both options
    from `winrt`, not from top-level kwargs."""
    src = inspect.getsource(_winrt_backend_cls().__init__)
    assert 'winrt.get("use_cached_services")' in src
    assert 'winrt.get("address_type")' in src


def test_nested_winrt_args_reach_the_backend():
    backend = _winrt_backend_cls()(
        "AA:BB:CC:DD:EE:FF",
        services=None,
        winrt={"address_type": "random", "use_cached_services": False},
        timeout=10.0,          # the backend reads this out of kwargs directly
    )
    assert backend._use_cached_services is False
    assert backend._address_type == "random"


def test_top_level_kwargs_are_silently_ignored():
    """Documents the trap directly: the old call style leaves the cache enabled.

    If a future bleak starts honouring or rejecting top-level kwargs, this test
    fails and the comment in the daemon can be simplified.
    """
    backend = _winrt_backend_cls()(
        "AA:BB:CC:DD:EE:FF",
        services=None,
        winrt={},
        timeout=10.0,
        address_type="random",
        use_cached_services=False,
    )
    assert backend._use_cached_services is None, (
        "bleak now honours top-level kwargs; the daemon's nesting workaround "
        "and its comment can be revisited"
    )
    assert backend._address_type is None


def _strip_comments(src: str) -> str:
    """Drop comment lines so the guard below inspects CODE, not prose.

    Without this, the daemon's own explanatory comment about the dead top-level
    form trips the negative assertions.
    """
    return "\n".join(
        line for line in src.splitlines() if not line.strip().startswith("#")
    )


def test_daemon_nests_the_cache_bypass():
    """The actual regression guard: the daemon's own call site must nest it."""
    from daemon import claude_usage_daemon_windows as d

    code = _strip_comments(inspect.getsource(d.connect_and_run))
    assert "winrt={" in code, "BleakClient options are no longer nested under winrt="
    assert '"use_cached_services": False' in code
    # The dead top-level form must not creep back in.
    assert "use_cached_services=False" not in code


def test_daemon_does_not_force_address_type():
    """address_type must stay unset.

    It was long passed as "random" but only as a dead top-level kwarg, so the
    configuration that actually works in the field is "let the OS decide".
    Honouring "random" broke connect outright: this board's address is the
    factory-burned MAC (public), and WinRT then finds no device at all.
    """
    from daemon import claude_usage_daemon_windows as d

    code = _strip_comments(inspect.getsource(d.connect_and_run))
    assert "address_type" not in code, (
        "address_type is set again — if that is deliberate, confirm the board's "
        "address really is random, because forcing the wrong type surfaces as "
        "BleakDeviceNotFoundError rather than as a type mismatch"
    )
