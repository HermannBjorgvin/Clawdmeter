"""Extra displays the daemon can push the same payload to.

`collectors/` is the input seam -- a provider you poll, normalized into an
`UsageSnapshot`. This is the output seam: somewhere the resulting payload gets
shown. The device on your desk is the primary one and is NOT a sink; it owns
the BLE link, the refresh nudge, and the poll clock. Everything here is a
secondary display that gets told what the device was just told.

Two rules, and they are the same rule the second *provider* follows: a sink
never gates the device, and a sink never takes the daemon down.

  * Its result does not advance `last_poll`. A Busy Bar that has been unplugged
    must not throttle the meter on your desk, and one that answers instantly
    must not excuse a failed BLE write.
  * Its exceptions never leave `publish()`. A sink is someone else's HTTP
    server on someone else's network; it will time out, 404 after a firmware
    update, and vanish when the Wi-Fi drops.

Sinks are opt-in through the config file and cost nothing when unconfigured --
`active_sinks()` returns empty and `publish()` is a no-op.
"""

from __future__ import annotations

from typing import Protocol, runtime_checkable


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
        _sinks = []          # populated as sinks are added
    return _sinks


async def publish(payload: dict, log=print) -> None:
    """Fan the payload out to every configured sink. Never raises.

    Failures are logged once per occurrence rather than swallowed silently --
    a display that has quietly stopped updating is the whole failure mode this
    project exists to avoid.
    """
    for sink in active_sinks():
        try:
            if not await sink.show(payload):
                log(f"{sink.name}: update failed")
        except Exception as exc:                # noqa: BLE001 -- see module docstring
            log(f"{sink.name}: {type(exc).__name__}: {exc}")
