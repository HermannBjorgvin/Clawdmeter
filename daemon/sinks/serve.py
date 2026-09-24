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


def update(payload: dict) -> None:
    """Remember what the device was last told. Cheap; called every poll."""
    global _latest
    s_pct = payload.get("s")
    resets_in = payload.get("sr")
    body: dict = {
        "ok": bool(payload.get("ok")) and payload.get("has_s", True),
        "pct": float(s_pct or 0.0),
    }
    # An absolute instant, not a duration: the app hands it straight to the
    # device's countdown element, which then stays true without being told
    # again. Converted here because the host is the one that knows the clock.
    if isinstance(resets_in, int) and resets_in >= 0:
        body["resets_at"] = int(time.time() + resets_in * 60)
    _latest = body


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
