#!/usr/bin/env python3
"""Tests for the output seam and the BUSY Bar sink.

The device on the desk is primary. Everything here guards the two rules that
keep a secondary display from becoming one: it cannot gate the poll clock, and
it cannot take the daemon down.

Run: python -m pytest daemon/tests/test_sinks.py -x -q
"""
import asyncio
import json

import pytest

from daemon import sinks
from daemon.sinks.busybar import BusyBarSink, elements_for


LIVE = {"s": 34.0, "sr": 281, "ok": True, "has_s": True}


def _by_id(body):
    return {e["id"]: e for e in body["elements"]}


# --- layout -----------------------------------------------------------------

def test_bar_fill_is_proportional_and_colored_like_the_firmware():
    """One meter, two displays: the thresholds have to agree or the bar and the
    device disagree about the same number."""
    assert _by_id(elements_for({**LIVE, "s": 50.0}))["fill"]["color"] == "#8FA76BFF"
    assert _by_id(elements_for({**LIVE, "s": 80.0}))["fill"]["color"] == "#D97757FF"
    assert _by_id(elements_for({**LIVE, "s": 95.0}))["fill"]["color"] == "#C0392BFF"

    fill = _by_id(elements_for({**LIVE, "s": 50.0}))["fill"]
    assert fill["width"] == 36                      # half of 72
    assert fill["x"] == 0 and fill["width"] <= 72


def test_a_nonzero_percent_always_lights_at_least_one_pixel():
    """1% of 72px rounds to zero. Showing nothing there would read as "no data"
    when it actually means "you have started"."""
    assert _by_id(elements_for({**LIVE, "s": 0.4}))["fill"]["width"] == 1


def test_zero_percent_draws_a_track_and_no_fill():
    body = _by_id(elements_for({**LIVE, "s": 0.0}))
    assert "track" in body and "fill" not in body


def test_countdown_carries_an_absolute_timestamp():
    """The device counts down by itself, so it needs the instant, not the
    remaining minutes -- that is what keeps it true between polls."""
    body = _by_id(elements_for({**LIVE, "sr": 120}, now=1_000_000.0))
    assert body["reset"]["timestamp"] == str(1_000_000 + 120 * 60)
    assert body["reset"]["direction"] == "time_left"


def test_a_missing_window_omits_the_countdown_rather_than_faking_one():
    """A frozen countdown is worse than no countdown: it is the number people
    act on, and a stuck one is indistinguishable from a live one."""
    assert "reset" not in _by_id(elements_for({**LIVE, "sr": -1}))
    assert "reset" not in _by_id(elements_for({"s": 34.0, "ok": True}))


def test_no_data_says_so_instead_of_drawing_an_empty_bar():
    body = _by_id(elements_for({"ok": False}))
    assert body["pct"]["text"] == "no data"
    assert "track" not in body and "fill" not in body


def test_text_is_ascii_only_like_the_api_demands():
    """The draw API rejects anything outside \\x20-\\x7E, the same constraint the
    firmware's subset fonts impose."""
    for el in elements_for(LIVE)["elements"]:
        if el["type"] == "text":
            assert all(0x20 <= ord(c) <= 0x7E for c in el["text"])


def test_the_led_blinks_only_when_the_bar_alone_would_not_do():
    assert "led_notification_color" not in elements_for({**LIVE, "s": 80.0})
    assert elements_for({**LIVE, "s": 95.0})["led_notification_color"] == "#C0392BFF"


def test_draw_priority_stays_below_a_busy_session():
    """A focus session draws at 90. Going above it would let a usage widget
    overwrite the message the device exists to show."""
    assert elements_for(LIVE)["priority"] < 90


def test_the_request_is_json_serializable():
    json.dumps(elements_for(LIVE))          # httpx will do this; fail here first


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


# --- transport --------------------------------------------------------------

def test_a_focus_session_preempting_the_draw_is_not_an_error(monkeypatch):
    """409 means a higher-priority app owns the screen. Expected, not a fault --
    logging it as one would cry wolf every Pomodoro."""
    class _Resp:
        status_code = 409

    class _Client:
        async def __aenter__(self): return self
        async def __aexit__(self, *a): return False
        async def post(self, *a, **k): return _Resp()

    monkeypatch.setattr("httpx.AsyncClient", lambda **k: _Client())
    assert asyncio.run(BusyBarSink("http://bar.local").show(LIVE)) is True


def test_from_config_is_off_unless_a_url_is_set(monkeypatch):
    import daemon.claude_usage_daemon as mod
    from daemon.sinks import busybar

    monkeypatch.setattr(mod, "read_config_value",
                        lambda key, **kw: "" if key == "busybar_url" else "")
    assert busybar.from_config() is None

    monkeypatch.setattr(mod, "read_config_value",
                        lambda key, **kw: "http://bar.local/" if key == "busybar_url"
                        else "tok")
    sink = busybar.from_config()
    assert sink.base_url == "http://bar.local"      # trailing slash trimmed
    assert sink.token == "tok"


# --- a sink must not delay the device either --------------------------------

def test_a_slow_sink_does_not_hold_up_the_device_write(monkeypatch):
    """Not hypothetical: the Busy Bar answers a draw in ~5s. Awaited inline
    that would have put five seconds on every poll of the primary device."""
    import daemon.claude_usage_daemon as mod

    started = asyncio.Event()

    class _Slow:
        name = "slow"

        async def show(self, payload):
            started.set()
            await asyncio.sleep(30)
            return True

    class _Client:
        async def write_gatt_char(self, *a, **k):
            return None

    monkeypatch.setattr(sinks, "_sinks", [_Slow()])
    monkeypatch.setattr(sinks, "_inflight", None)

    async def scenario():
        session = mod.Session(_Client())
        ok = await asyncio.wait_for(session.write_payload(LIVE), timeout=1)
        await asyncio.wait_for(started.wait(), timeout=1)   # it really did start
        sinks._inflight.cancel()
        return ok

    assert asyncio.run(scenario()) is True


def test_an_update_still_in_flight_skips_rather_than_queues(monkeypatch):
    """These are current-state displays. A backlog of stale frames helps
    nobody, and an unreachable sink would grow one every poll."""
    calls = []

    class _Slow:
        name = "slow"

        async def show(self, payload):
            calls.append(payload)
            await asyncio.sleep(30)
            return True

    monkeypatch.setattr(sinks, "_sinks", [_Slow()])
    monkeypatch.setattr(sinks, "_inflight", None)
    lines = []

    async def scenario():
        sinks.publish_soon(LIVE, log=lines.append)
        await asyncio.sleep(0)
        sinks.publish_soon(LIVE, log=lines.append)
        await asyncio.sleep(0)
        sinks._inflight.cancel()

    asyncio.run(scenario())
    assert len(calls) == 1
    assert any("skipping" in l for l in lines)


def test_the_busy_bar_timeout_clears_its_measured_response_time():
    """The first live attempt died on a 5s ConnectTimeout against a device
    that answers in 5.0s. A timeout at the response time is a coin flip."""
    from daemon.sinks import busybar

    assert busybar.HTTP_TIMEOUT >= 15.0


def test_the_draw_path_is_the_devices_prefix_not_the_clouds():
    """api.busy.app documents /busybar/...; that is the CLOUD relay's
    namespace. The bar itself serves /api/..., and posting to the spec's path
    reaches its web-UI file server, which answers 405 Allow: GET -- an error
    that reads like "wrong method" and actually means "wrong prefix"."""
    from daemon.sinks import busybar

    assert busybar.DRAW_PATH == "/api/display/draw"
    assert not busybar.DRAW_PATH.startswith("/busybar/")
