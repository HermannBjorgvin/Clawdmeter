"""Extra displays the daemon can push the same payload to.

`collectors/` is the input seam -- a provider you poll, normalized into an
`UsageSnapshot`. This is the output seam: somewhere the resulting payload gets
shown. The device on your desk is the primary one and is NOT a sink; it owns
the BLE link, the refresh nudge, and the poll clock. Everything here is a
secondary display that gets told what the device was just told.

Three rules, and they are the same rule the second *provider* follows: a sink
never gates the device, never delays it, and never takes the daemon down.

  * Its result does not advance `last_poll`. A Busy Bar that has been unplugged
    must not throttle the meter on your desk, and one that answers instantly
    must not excuse a failed BLE write.
  * It runs off the write path. `publish_soon()` schedules the fan-out and
    returns, because a slow sink is not a hypothetical: the Busy Bar answers
    in ~5s, which awaited inline would have added five seconds to every poll
    of the primary device. An update still in flight when the next one is
    ready is skipped rather than queued -- these are current-state displays,
    and a backlog of stale frames helps nobody.
  * Its exceptions never leave `publish()`. A sink is someone else's HTTP
    server on someone else's network; it will time out, 404 after a firmware
    update, and vanish when the Wi-Fi drops.

Sinks are opt-in through the config file and cost nothing when unconfigured --
`active_sinks()` returns empty and `publish()` is a no-op.
"""

from __future__ import annotations

import asyncio
from typing import Protocol, runtime_checkable

from . import serve


@runtime_checkable
class Sink(Protocol):
    """A secondary display."""

    name: str

    async def show(self, payload: dict) -> bool:
        """Render `payload`. Returns success, for logging only."""
        ...


_sinks: list[Sink] | None = None


def active_sinks(reload: bool = False) -> list[Sink]:
    """Every sink the config file enables. Built once, then cached."""
    global _sinks
    if _sinks is None or reload:
        from .busybar import from_config as _busybar_from_config
        _sinks = [s for s in (_busybar_from_config(),) if s is not None]
    return _sinks


_inflight: "asyncio.Task | None" = None


def publish_soon(payload: dict, log=print) -> None:
    """Start the fan-out and return. Never raises, never blocks the caller."""
    global _inflight
    if not active_sinks():
        serve.update(payload)      # the pull path runs with no sink configured
        return
    if _inflight is not None and not _inflight.done():
        log("sinks: previous update still in flight, skipping this one")
        return
    _inflight = asyncio.create_task(publish(payload, log=log))
    # publish() swallows per-sink failures, so a task exception means the
    # fan-out itself broke. Surface it rather than letting asyncio report an
    # unretrieved exception at some unrelated moment.
    _inflight.add_done_callback(
        lambda t: t.cancelled() or t.exception() is None
        or log(f"sinks: publish crashed: {t.exception()!r}"))


async def publish(payload: dict, log=print) -> None:
    """Fan the payload out to every configured sink. Never raises.

    Failures are logged once per occurrence rather than swallowed silently --
    a display that has quietly stopped updating is the whole failure mode this
    project exists to avoid.
    """
    serve.update(payload)          # cheap, in-memory; for the device-side app
    for sink in active_sinks():
        try:
            if not await sink.show(payload):
                log(f"{sink.name}: update failed")
        except Exception as exc:                # noqa: BLE001 -- see module docstring
            log(f"{sink.name}: {type(exc).__name__}: {exc}")
