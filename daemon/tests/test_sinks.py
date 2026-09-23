#!/usr/bin/env python3
"""Tests for the output seam and the BUSY Bar sink.

The device on the desk is primary. Everything here guards the two rules that
keep a secondary display from becoming one: it cannot gate the poll clock, and
it cannot take the daemon down.

Run: python -m pytest daemon/tests/test_sinks.py -x -q
"""
import asyncio

import pytest

from daemon import sinks


LIVE = {"s": 34.0, "sr": 281, "ok": True, "has_s": True}


# --- the two rules ----------------------------------------------------------

class _Boom:
    name = "boom"

    async def show(self, payload):
        raise RuntimeError("network on fire")


class _Sad:
    name = "sad"

    async def show(self, payload):
        return False


def test_a_sink_that_explodes_cannot_take_the_daemon_down(monkeypatch):
    """A sink is someone else's HTTP server on someone else's network."""
    monkeypatch.setattr(sinks, "_sinks", [_Boom(), _Sad()])
    lines = []
    asyncio.run(sinks.publish(LIVE, log=lines.append))
    assert any("network on fire" in l for l in lines)
    assert any("update failed" in l for l in lines)


def test_unconfigured_sinks_cost_nothing(monkeypatch):
    monkeypatch.setattr(sinks, "_sinks", [])
    asyncio.run(sinks.publish(LIVE, log=lambda _: pytest.fail("logged nothing-ness")))


def test_a_dead_sink_does_not_change_what_the_device_write_reports(monkeypatch):
    """The whole point of the seam: write_payload returns the DEVICE's result,
    which is what gates the poll clock. An unplugged Busy Bar must not throttle
    the meter on the desk."""
    import daemon.claude_usage_daemon as mod

    class _Client:
        async def write_gatt_char(self, *a, **k):
            return None

    monkeypatch.setattr(sinks, "_sinks", [_Boom()])
    session = mod.Session(_Client())
    assert asyncio.run(session.write_payload(LIVE)) is True
