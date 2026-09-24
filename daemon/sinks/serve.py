"""Serve the latest payload so a device-side app can pull it.

The BUSY Bar sink pushes; this lets the bar's own app pull. The difference
matters at the moment you *look* at the device: a push shows whatever the
daemon last sent, a pull shows what is true now, which is what you want from
something you selected out of a menu.

Deliberately not a web server. It answers exactly one path, holds one value in
memory, writes no files and takes no input beyond the request line. A daemon
that reads your OAuth tokens has no business growing a framework.

BINDING. Over USB the bar is 10.0.4.20 and the host is 10.0.4.21, both fixed,
so the default binds to that interface alone -- not 0.0.0.0. The payload is
your usage, which is nobody else's business on a coffee-shop network, and
binding narrow is cheaper than explaining to a firewall later. Override with
`busybar_serve` in the config if the bar talks over Wi-Fi instead.
"""

from __future__ import annotations

import asyncio
import json
import time

DEFAULT_BIND = "10.0.4.21"       # this host, on the bar's USB network
DEFAULT_PORT = 8724
PATH = "/usage.json"

_latest: dict = {"ok": False}
_server: asyncio.AbstractServer | None = None


def _cards(payload: dict, provider: str, now: float) -> list[dict]:
    """Every quota one provider is metering, in the order it should be shown.

    One card per bar, each carrying its own label, because a percentage with
    no name on a 72x16 screen is just a number -- the device's own UI has the
    same problem and solves it with the pill.
    """
    out: list[dict] = []

    def add(label: str, pct, resets_in) -> None:
        if pct is None:
            return
        card = {"provider": provider, "label": label, "pct": float(pct)}
        if isinstance(resets_in, int) and resets_in >= 0:
            # Seconds remaining at the moment of this poll, not an absolute
            # instant. The app ages it with its own elapsed time, so the
            # countdown never depends on the bar's clock being right.
            card["in_s"] = resets_in * 60
        out.append(card)

    session_label = payload.get("sm") or "Current"
    weekly_label = payload.get("wm") or "Weekly"
    if payload.get("has_s", True):
        add(session_label, payload.get("s"), payload.get("sr"))
    if payload.get("has_w", True):
        add(weekly_label, payload.get("w"), payload.get("wr"))

    # Scoped model allowances share the weekly reset instant.
    for scoped in payload.get("ws") or []:
        if isinstance(scoped, dict) and scoped.get("n"):
            add(scoped["n"], scoped.get("p"), payload.get("wr"))

    # Reset credits are a count, not a proportion, so they ride as their own
    # shape rather than being forced into a bar.
    held, used = payload.get("rc"), payload.get("ru")
    if held or used:
        card = {"provider": provider, "label": "Resets",
                "held": int(held or 0), "used": int(used or 0)}
        if isinstance(payload.get("rm"), int) and payload["rm"] >= 0:
            card["in_s"] = payload["rm"] * 60
        out.append(card)
    return out


def update(payload: dict) -> None:
    """Remember what the device was last told. Cheap; called every poll."""
    global _latest
    now = time.time()
    cards = _cards(payload, "claude", now)
    codex = payload.get("x")
    if isinstance(codex, dict) and codex.get("ok"):
        cards += _cards(codex, "codex", now)

    _latest = {"ok": bool(payload.get("ok")) or bool(cards), "cards": cards}


async def _handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    try:
        request = await asyncio.wait_for(reader.readline(), timeout=5)
        parts = request.decode("latin-1").split()
        ok = len(parts) >= 2 and parts[0] == "GET" and parts[1].split("?")[0] == PATH
        body = json.dumps(_latest).encode() if ok else b'{"error":"not found"}'
        status = "200 OK" if ok else "404 Not Found"
        writer.write(
            f"HTTP/1.1 {status}\r\nContent-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\nConnection: close\r\n"
            "Access-Control-Allow-Origin: *\r\n\r\n".encode() + body)
        await writer.drain()
    except (asyncio.TimeoutError, ConnectionError, UnicodeDecodeError):
        pass
    finally:
        writer.close()


async def start(bind: str = DEFAULT_BIND, port: int = DEFAULT_PORT,
                log=print) -> bool:
    """Start serving. False (never an exception) if the address is unusable."""
    global _server
    if _server is not None:
        return True
    try:
        _server = await asyncio.start_server(_handle, bind, port)
    except OSError as exc:
        # Almost always "Can't assign requested address" because the bar is
        # not plugged in, so the USB interface does not exist. Not fatal, and
        # not worth retrying on a timer -- plug it in and restart.
        log(f"busybar serve: not listening on {bind}:{port} ({exc.strerror})")
        _server = None
        return False
    log(f"busybar serve: {bind}:{port}{PATH}")
    return True


async def stop() -> None:
    global _server
    if _server is not None:
        _server.close()
        await _server.wait_closed()
        _server = None
