#!/usr/bin/env python3
"""Tests for the poll schedule being independent of the BLE link — UsagePoller.

Polling used to live inside connect_and_run, so the daemon only recorded the
account's rate-limit utilisation while the hardware happened to be connected.
Those observations are the ground truth behind usage_history.observe() (each
5-hour window's measured peak, and the tokens-per-percent calibration fitted
from it), so unplugging the device and carrying on working corrupted the
history, not just the live display. Polling is now one background task that
runs for the daemon's whole life; connect_and_run only transmits what it has
published.

httpx is fully mocked here — no test makes a live network call, and no test
reads the developer's own credentials or config.

Run: python -m pytest daemon/tests/test_poll_scheduling.py -x -q
"""
import asyncio
import json
import time
from pathlib import Path
from unittest.mock import AsyncMock, MagicMock, patch

import pytest

import daemon.claude_usage_daemon as mod


def _run(coro):
    """Run a coroutine on a private loop, then assert nothing was left behind.

    A fresh loop per test keeps these order-independent in the full suite (same
    helper as test_usage_endpoint.py); the leftover-task check is what proves
    the poll task never outlives the daemon.
    """
    loop = asyncio.new_event_loop()
    try:
        result = loop.run_until_complete(coro)
        pending = [t for t in asyncio.all_tasks(loop) if not t.done()]
        assert not pending, f"orphaned tasks after shutdown: {pending}"
        return result
    finally:
        loop.close()


@pytest.fixture(autouse=True)
def _isolated(tmp_path, monkeypatch):
    """Detach every test from the real machine: no user config, no Keychain, no
    transcript scan, and a poll schedule measured in milliseconds."""
    monkeypatch.setattr(mod, "CONFIG_FILE", tmp_path / "config")      # absent
    monkeypatch.setattr(mod, "read_config_dirs", lambda: [Path("/fake/dir")])
    monkeypatch.setattr(mod, "read_token_for", lambda d: "tok")
    monkeypatch.setattr(mod, "read_history_setting", lambda: "off")   # no scan
    monkeypatch.setattr(mod, "POLL_INTERVAL", 0.02)
    monkeypatch.setattr(mod, "TICK", 0.01)
    monkeypatch.setattr(mod, "PAYLOAD_MAX_AGE_S", 10.0)
    monkeypatch.setattr(mod, "_usage_endpoint_benched_until", 0.0)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _connected_client(on_write=None):
    """A BleakClient stand-in that is already connected and records writes."""
    client = AsyncMock()
    client.connect = AsyncMock(return_value=None)
    client.is_connected = True
    client.disconnect = AsyncMock()
    client.start_notify = AsyncMock()
    client.write_gatt_char = AsyncMock(side_effect=on_write)
    return client


def _target():
    device = MagicMock()
    device.address = "AA:BB:CC:DD:EE:FF"
    return device


def _capture_pollers(monkeypatch) -> list:
    """Swap in a UsagePoller subclass that records its instances, so a test can
    reach the poller main() built for itself (and its stop_event)."""
    made: list = []

    class _Recording(mod.UsagePoller):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, **kwargs)
            made.append(self)

    monkeypatch.setattr(mod, "UsagePoller", _Recording)
    return made


async def _until(predicate, timeout: float = 2.0) -> None:
    """Yield to the loop until ``predicate()`` is true, or fail the test."""
    deadline = time.monotonic() + timeout
    while not predicate():
        if time.monotonic() > deadline:
            raise AssertionError("condition not reached within timeout")
        await asyncio.sleep(0.005)


class _HttpClient:
    """Stands in for httpx.AsyncClient, recording every GET/POST it serves."""

    def __init__(self, get_status=200, get_body=None, post_status=200):
        self.get_status = get_status
        self.get_body = get_body or {}
        self.post_status = post_status
        self.gets: list = []
        self.posts: list = []

    async def __aenter__(self):
        return self

    async def __aexit__(self, *exc):
        return False

    def _resp(self, status, body):
        resp = MagicMock()
        resp.status_code = status
        resp.text = "mocked"
        resp.json = MagicMock(return_value=body)
        resp.headers = MagicMock()
        resp.headers.get = lambda name, default=None: default
        return resp

    async def get(self, url, **kwargs):
        self.gets.append(url)
        return self._resp(self.get_status, self.get_body)

    async def post(self, url, **kwargs):
        self.posts.append(url)
        return self._resp(self.post_status, {})


# ---------------------------------------------------------------------------
# 1. Polling with no device — the whole point of the change
# ---------------------------------------------------------------------------

def test_polls_with_no_device_ever_connected(monkeypatch):
    """The device is never found, yet the schedule keeps producing readings."""
    made = _capture_pollers(monkeypatch)
    polls: list = []

    async def fake_poll_api(token):
        polls.append(token)
        if len(polls) >= 3 and made:
            made[0].stop_event.set()
        return {"s": len(polls), "ok": True}

    connect = AsyncMock(side_effect=AssertionError("nothing should connect"))
    monkeypatch.setattr(mod, "discover_target", AsyncMock(return_value=None))
    monkeypatch.setattr(mod, "connect_and_run", connect)
    monkeypatch.setattr(mod, "poll_api", fake_poll_api)

    _run(mod.main())

    assert len(polls) >= 3, "polling stopped when no device was present"
    connect.assert_not_called()


def test_polling_continues_across_a_disconnect(monkeypatch):
    """Readings keep coming after the link drops — the gap this change closes."""
    made = _capture_pollers(monkeypatch)
    polls: list = []
    timeline: list = []

    async def fake_poll_api(token):
        polls.append(token)
        if len(polls) >= 6 and made:
            made[0].stop_event.set()
        return {"s": len(polls), "ok": True}

    seen_device = []

    async def fake_discover(skip_addr=None):
        # One connectable device, then it is gone for good.
        if seen_device:
            return None
        seen_device.append(True)
        return _target()

    async def fake_connect_and_run(target, stop_event, poller):
        timeline.append(("connected", len(polls)))
        await asyncio.sleep(mod.POLL_INTERVAL * 3)   # hold the link a while
        timeline.append(("disconnected", len(polls)))
        return True

    monkeypatch.setattr(mod, "discover_target", fake_discover)
    monkeypatch.setattr(mod, "connect_and_run", fake_connect_and_run)
    monkeypatch.setattr(mod, "poll_api", fake_poll_api)

    _run(mod.main())

    assert [e[0] for e in timeline] == ["connected", "disconnected"]
    at_connect, at_disconnect = timeline[0][1], timeline[1][1]
    assert at_disconnect > at_connect, "no polls happened while connected"
    assert len(polls) > at_disconnect, "polling stopped at the disconnect"


def test_attach_history_runs_on_the_poll_schedule_not_the_send_schedule(monkeypatch):
    """attach_history is what calls observe(); it must run per poll, with or
    without a device, or the calibration stops learning while unplugged."""
    attached: list = []

    async def spy_attach(payload):
        attached.append(dict(payload))

    async def go():
        stop = asyncio.Event()
        poller = mod.UsagePoller(stop)
        polled = AsyncMock(return_value={"s": 7, "ok": True})
        with patch.object(mod, "attach_history", spy_attach), \
             patch.object(mod, "poll_api", new=polled):
            task = asyncio.create_task(poller.run())
            await _until(lambda: len(attached) >= 2)
            stop.set()
            await task

    _run(go())
    assert len(attached) >= 2
    assert all(a["s"] == 7 for a in attached), "history attached to the wrong dict"


# ---------------------------------------------------------------------------
# 2. Transmitting what the poller published
# ---------------------------------------------------------------------------

def test_connect_sends_the_cached_payload_without_waiting_for_a_poll(monkeypatch):
    """A device that connects mid-cycle gets the cache immediately rather than
    a blank screen until the next scheduled poll."""
    # A schedule far longer than the test could ever wait for: any write here
    # can only have come from the cache.
    monkeypatch.setattr(mod, "POLL_INTERVAL", 30.0)
    monkeypatch.setattr(mod, "TICK", 30.0)
    writes: list = []

    async def go():
        stop = asyncio.Event()
        poller = mod.UsagePoller(stop)
        poller._publish({"s": 42, "ok": True})   # published before this connect

        async def cap_write(uuid, data, response=False):
            writes.append(json.loads(data.decode()))
            stop.set()

        client = _connected_client(cap_write)
        with patch.object(mod, "BleakClient", return_value=client), \
             patch.object(mod, "poll_api",
                          new=AsyncMock(side_effect=AssertionError("must not poll"))):
            return await mod.connect_and_run(_target(), stop, poller)

    t0 = time.monotonic()
    used = _run(go())
    assert time.monotonic() - t0 < 5.0, "the connect waited for the poll schedule"
    assert writes == [{"s": 42, "ok": True}]
    assert used is True


def test_stale_cache_is_not_pushed_to_a_connecting_device(monkeypatch):
    """Past PAYLOAD_MAX_AGE_S the last polls have been failing; the device is
    better off on its own idle screen than on numbers we know are wrong."""
    monkeypatch.setattr(mod, "PAYLOAD_MAX_AGE_S", 1.0)
    writes: list = []

    async def go():
        stop = asyncio.Event()
        poller = mod.UsagePoller(stop)
        poller._publish({"s": 42, "ok": True})
        poller.published_at = time.time() - 3600   # an hour old

        async def cap_write(uuid, data, response=False):
            writes.append(json.loads(data.decode()))

        client = _connected_client(cap_write)
        with patch.object(mod, "BleakClient", return_value=client), \
             patch.object(mod, "poll_api",
                          new=AsyncMock(side_effect=AssertionError("must not poll"))):
            task = asyncio.create_task(mod.connect_and_run(_target(), stop, poller))
            await asyncio.sleep(0.1)
            stop.set()
            await task

    _run(go())
    assert writes == []


def test_transient_poll_failure_stays_silent(monkeypatch):
    """A live token that didn't answer this cycle publishes nothing, so the
    device keeps its last screen instead of being idled."""
    writes: list = []

    async def go():
        stop = asyncio.Event()
        poller = mod.UsagePoller(stop)

        async def cap_write(uuid, data, response=False):
            writes.append(json.loads(data.decode()))

        client = _connected_client(cap_write)
        with patch.object(mod, "BleakClient", return_value=client), \
             patch.object(mod, "poll_active", new=AsyncMock(return_value=(None, False))):
            poll_task = asyncio.create_task(poller.run())
            ble_task = asyncio.create_task(mod.connect_and_run(_target(), stop, poller))
            await asyncio.sleep(0.1)
            stop.set()
            await asyncio.gather(poll_task, ble_task)

    _run(go())
    assert writes == [], "a transient failure must not reach the device"


# ---------------------------------------------------------------------------
# 3. Refresh requests must not double-poll
# ---------------------------------------------------------------------------

def test_refresh_during_an_in_flight_poll_does_not_start_a_second(monkeypatch):
    """Exactly one task polls, and the nudge flag is cleared after the cycle, so
    a refresh arriving mid-poll is answered by that poll rather than queueing
    another one behind it."""
    monkeypatch.setattr(mod, "POLL_INTERVAL", 30.0)   # no scheduled re-poll
    state = {"calls": 0, "in_flight": 0, "max_in_flight": 0}

    async def go():
        stop = asyncio.Event()
        gate = asyncio.Event()
        poller = mod.UsagePoller(stop)

        async def fake_poll_active(selector):
            state["calls"] += 1
            state["in_flight"] += 1
            state["max_in_flight"] = max(state["max_in_flight"], state["in_flight"])
            await gate.wait()
            state["in_flight"] -= 1
            return {"s": 1, "ok": True}, False

        with patch.object(mod, "poll_active", fake_poll_active):
            task = asyncio.create_task(poller.run())
            await _until(lambda: state["in_flight"] == 1)
            poller.request_poll()          # the firmware's nudge, mid-poll
            gate.set()
            await asyncio.sleep(0.1)       # plenty of room for a second poll
            stop.set()
            await task

    _run(go())
    assert state["max_in_flight"] == 1, "two polls ran concurrently"
    assert state["calls"] == 1, "the mid-flight refresh queued a second poll"


def test_refresh_while_idle_polls_early(monkeypatch):
    """The nudge is still a nudge: during the sleep it brings the next poll
    forward instead of waiting out POLL_INTERVAL."""
    monkeypatch.setattr(mod, "POLL_INTERVAL", 30.0)
    calls: list = []

    async def go():
        stop = asyncio.Event()
        poller = mod.UsagePoller(stop)

        async def fake_poll_active(selector):
            calls.append(time.monotonic())
            return {"s": 1, "ok": True}, False

        with patch.object(mod, "poll_active", fake_poll_active):
            task = asyncio.create_task(poller.run())
            await _until(lambda: len(calls) == 1)
            poller.request_poll()
            await _until(lambda: len(calls) == 2)   # long before POLL_INTERVAL
            stop.set()
            await task

    _run(go())
    assert len(calls) == 2


def test_device_refresh_resends_and_nudges_the_poller(monkeypatch):
    """A refresh from the firmware answers from the cache straight away (the
    device asks because it has nothing to show) and asks for a fresh reading."""
    monkeypatch.setattr(mod, "POLL_INTERVAL", 30.0)
    writes: list = []

    async def go():
        stop = asyncio.Event()
        poller = mod.UsagePoller(stop)
        poller._publish({"s": 5, "ok": True})

        async def cap_write(uuid, data, response=False):
            writes.append(json.loads(data.decode()))
            if len(writes) >= 2:
                stop.set()

        client = _connected_client(cap_write)
        with patch.object(mod, "BleakClient", return_value=client), \
             patch.object(mod, "poll_api",
                          new=AsyncMock(side_effect=AssertionError("must not poll"))):
            task = asyncio.create_task(mod.connect_and_run(_target(), stop, poller))
            await _until(lambda: len(writes) == 1)      # the cache, on connect
            # The Session built inside connect_and_run owns the event the
            # firmware's notify sets; fire the callback it subscribed with,
            # exactly as bleak would on a REQ notification.
            await _until(lambda: client.start_notify.await_count == 1)
            _char, callback = client.start_notify.await_args.args
            callback(None, bytearray(b"\x01"))
            await _until(lambda: len(writes) == 2)
            await task
        return poller

    poller = _run(go())
    assert writes == [{"s": 5, "ok": True}, {"s": 5, "ok": True}]
    assert poller._wake.is_set(), "the refresh never reached the poll schedule"


# ---------------------------------------------------------------------------
# 4. Dead tokens: the no-data beat survives, and nothing hot-loops
# ---------------------------------------------------------------------------

def test_dead_token_beat_reaches_a_connected_device(monkeypatch):
    """A 401 on every config dir still produces the {"ok": false} beat, now
    published by the poller and transmitted by the BLE loop."""
    writes: list = []
    http = _HttpClient(get_status=401)

    async def go():
        stop = asyncio.Event()
        poller = mod.UsagePoller(stop)

        async def cap_write(uuid, data, response=False):
            writes.append(json.loads(data.decode()))
            stop.set()

        client = _connected_client(cap_write)
        with patch("httpx.AsyncClient", return_value=http), \
             patch.object(mod, "BleakClient", return_value=client):
            poll_task = asyncio.create_task(poller.run())
            ble_task = asyncio.create_task(mod.connect_and_run(_target(), stop, poller))
            await _until(lambda: writes)
            await asyncio.gather(poll_task, ble_task)

    _run(go())
    assert writes and writes[0] == {"ok": False}
    assert http.posts == [], "a 401 must not fall through to the token-costing POST"


def test_dead_token_does_not_hot_loop(monkeypatch):
    """TokenExpired is a steady state (only Claude Code can re-seed the token),
    so it must stay on the schedule rather than spinning."""
    monkeypatch.setattr(mod, "POLL_INTERVAL", 0.05)
    http = _HttpClient(get_status=401)

    async def go():
        stop = asyncio.Event()
        poller = mod.UsagePoller(stop)
        with patch("httpx.AsyncClient", return_value=http):
            started = time.monotonic()
            task = asyncio.create_task(poller.run())
            await asyncio.sleep(0.3)
            stop.set()
            await task
            return time.monotonic() - started

    elapsed = _run(go())
    ceiling = int(elapsed / 0.05) + 2      # one per interval, plus slack
    assert 2 <= len(http.gets) <= ceiling, (
        f"{len(http.gets)} polls in {elapsed:.2f}s — expected about "
        f"{elapsed / 0.05:.0f}"
    )


def test_missing_token_makes_no_network_call_at_all(monkeypatch):
    """No token anywhere is answered from disk: the schedule keeps running but
    nothing is sent to Anthropic."""
    monkeypatch.setattr(mod, "read_token_for", lambda d: None)
    http = _HttpClient()

    async def go():
        stop = asyncio.Event()
        poller = mod.UsagePoller(stop)
        with patch("httpx.AsyncClient", return_value=http):
            task = asyncio.create_task(poller.run())
            await _until(lambda: poller.seq >= 2)
            stop.set()
            await task
        return poller.payload

    payload = _run(go())
    assert payload == {"ok": False}
    assert http.gets == [] and http.posts == []


def test_usage_endpoint_bench_still_applies_on_the_new_schedule(monkeypatch):
    """The 429 cooldown is account-wide and survives the refactor: while benched
    the poller must not touch the endpoint, and falls back to the headers."""
    monkeypatch.setattr(mod, "_usage_endpoint_benched_until", time.time() + 900)
    http = _HttpClient()

    async def go():
        stop = asyncio.Event()
        poller = mod.UsagePoller(stop)
        with patch("httpx.AsyncClient", return_value=http):
            await poller.poll_once()

    _run(go())
    assert http.gets == [], "polled a benched endpoint"
    assert http.posts == [mod.API_URL], "did not fall back to the header path"


# ---------------------------------------------------------------------------
# 5. Shutdown
# ---------------------------------------------------------------------------

def test_shutdown_is_prompt_with_the_poll_task_running(monkeypatch):
    """SIGINT/SIGTERM sets stop_event; main must unwind at once and leave no
    task behind, even though the poller is parked on a long sleep."""
    monkeypatch.setattr(mod, "POLL_INTERVAL", 30.0)   # parked mid-sleep
    made = _capture_pollers(monkeypatch)
    monkeypatch.setattr(mod, "discover_target", AsyncMock(return_value=None))
    monkeypatch.setattr(mod, "connect_and_run",
                        AsyncMock(side_effect=AssertionError("nothing connects")))

    async def go():
        async def fake_poll_api(token):
            return {"s": 1, "ok": True}

        with patch.object(mod, "poll_api", fake_poll_api):
            main_task = asyncio.create_task(mod.main())
            await _until(lambda: bool(made) and made[0].seq >= 1)
            t0 = time.monotonic()
            made[0].stop_event.set()          # what the signal handler does
            await asyncio.wait_for(main_task, timeout=5)
            return time.monotonic() - t0, made[0]

    elapsed, poller = _run(go())      # _run also asserts no orphaned tasks
    assert elapsed < 2.0, f"shutdown took {elapsed:.2f}s"
    assert poller.stop_event.is_set()


def test_poll_loop_survives_a_broken_cycle(monkeypatch):
    """A bad cycle must not kill the task — dropping it would silently stop the
    history calibration for the rest of the daemon's life."""
    calls: list = []

    async def go():
        stop = asyncio.Event()
        poller = mod.UsagePoller(stop)

        async def flaky(selector):
            calls.append(1)
            if len(calls) == 1:
                raise RuntimeError("boom")
            return {"s": 3, "ok": True}, False

        with patch.object(mod, "poll_active", flaky):
            task = asyncio.create_task(poller.run())
            await _until(lambda: poller.seq >= 1)
            stop.set()
            await task
        return poller.payload

    assert _run(go()) == {"s": 3, "ok": True}
    assert len(calls) >= 2
