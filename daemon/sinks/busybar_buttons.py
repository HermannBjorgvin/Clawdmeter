"""Read the BUSY Bar's physical buttons, host-side.

The firmware on this device has no input API for JS -- `js_input.c`, which
installs the `listen` global, was committed five days after the 1.2.4 release
the bar runs -- so an app on the device cannot see a press at all. What the
device does have is a CLI on TCP 23 whose `input dump` command prints one line
per event:

    key: InputKeyStart type: InputTypePress

**It reads the pubsub, not the GUI**, so it reports presses the canvas has
already swallowed -- which is every press while our elements are on screen.
That is what makes this work rather than being a curiosity.

So the host owns the control state (see `serve.Control`) and the app renders
what it is told. When a newer firmware exposes `listen`, the app can post
events here instead and the state machine stays in one place.

THE RULES ARE THE SINKS' RULES, for the same reason: this shares a process
with the BLE writer that feeds the desk meter. Every await is bounded, the
task is never awaited by the poll loop, and no failure here escapes. A bar
that has been unplugged must not cost the meter on your desk anything.
"""

from __future__ import annotations

import asyncio
import re
import socket

from . import serve

CLI_PORT = 23
BANNER_S = 0.5          # let the shell print its banner before we type
CONNECT_TIMEOUT = 3.0
# No traffic for this long means the link is dead in a way keepalive missed.
# Physical silence is normal -- the guard is deliberately far longer than any
# gap between presses.
SILENCE_TIMEOUT = 300.0
BACKOFF_START = 1.0
BACKOFF_MAX = 30.0

_EVENT = re.compile(r"key: (InputKey\w+) type: (InputType\w+)")


def _apply(key: str, kind: str) -> bool:
    """Turn one event into a control change. True if anything moved.

    Short rather than Press for the buttons, so a long press stays available
    for something else later; Press for the wheel, which only ever emits it.
    """
    if key == "InputKeyStart" and kind == "InputTypeShort":
        serve.control.toggle_pause()
    elif key == "InputKeyOk" and kind == "InputTypeShort":
        serve.control.step(1)
    elif key == "InputKeyUp" and kind == "InputTypePress":
        serve.control.step(1)          # clockwise, matching js_input.c
    elif key == "InputKeyDown" and kind == "InputTypePress":
        serve.control.step(-1)
    else:
        return False
    return True


def _set_keepalive(writer: asyncio.StreamWriter) -> None:
    """Notice a half-open socket in ~25s instead of waiting out the silence
    guard. A rebooted bar leaves exactly that kind of socket behind."""
    sock = writer.get_extra_info("socket")
    if sock is None:
        return
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        for name, value in (("TCP_KEEPIDLE", 10), ("TCP_KEEPALIVE", 10),
                            ("TCP_KEEPINTVL", 5), ("TCP_KEEPCNT", 3)):
            opt = getattr(socket, name, None)
            if opt is not None:
                sock.setsockopt(socket.IPPROTO_TCP, opt, value)
    except OSError:
        pass                            # advisory only


async def _session(host: str, log) -> None:
    """One connection's worth of reading. Returns when it dies."""
    reader, writer = await asyncio.wait_for(
        asyncio.open_connection(host, CLI_PORT), timeout=CONNECT_TIMEOUT)
    try:
        _set_keepalive(writer)
        await asyncio.sleep(BANNER_S)
        writer.write(b"input dump\r\n")
        await writer.drain()
        log(f"busybar buttons: reading {host}:{CLI_PORT}")

        while True:
            line = await asyncio.wait_for(reader.readline(),
                                          timeout=SILENCE_TIMEOUT)
            if not line:                # EOF
                return
            # Everything else on this stream is banner, echo or ANSI.
            match = _EVENT.search(line.decode("latin-1", "replace"))
            if match:
                _apply(match.group(1), match.group(2))
    finally:
        writer.close()


async def run(host: str, log=print) -> None:
    """Keep a reader attached, forever. Never raises."""
    backoff = BACKOFF_START
    announced_loss = False
    while True:
        try:
            await _session(host, log)
            backoff = BACKOFF_START
            announced_loss = False
        except asyncio.CancelledError:
            raise
        except Exception as exc:        # noqa: BLE001 -- see module docstring
            # Logged once per outage, not once per attempt: an unplugged bar
            # would otherwise fill the log the daemon shares with the meter.
            if not announced_loss:
                # The message, not just the class: "OSError" alone sent me
                # looking for a closed port when the cause was elsewhere.
                log(f"busybar buttons: no CLI ({type(exc).__name__}: {exc}); "
                    f"retrying")
                announced_loss = True
        await asyncio.sleep(backoff)
        backoff = min(backoff * 2, BACKOFF_MAX)
