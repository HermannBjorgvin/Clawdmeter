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
CONTROL_PATH = "/control"

_latest: dict = {"ok": False}
_server: asyncio.AbstractServer | None = None

# How long a card holds the screen when nothing is pressed, and how often the
# rotation clock is checked.
CARD_S = 4.0
TICK_S = 0.25
MAX_WAIT_MS = 10_000


class Control:
    """What the device should be showing, and who says so.

    The rotation clock lives HERE rather than in the app, because the buttons
    do too: the firmware on this device has no input API for JS (js_input.c
    postdates its release), so physical presses can only be read host-side off
    the CLI. One state machine on one side beats two that have to agree.

    `gen` increments on every change and is what the long poll waits for.
    """

    def __init__(self) -> None:
        self.index = 0
        self.paused = False
        self.gen = 0
        self.n = 0
        self.dwell = time.monotonic()
        self._cond: asyncio.Condition | None = None

    def _bump(self) -> None:
        self.gen += 1
        cond = self._cond
        if cond is not None:
            # The waiters are in the same loop; schedule rather than await so
            # callers (including the button reader) never block on them.
            asyncio.get_running_loop().create_task(self._notify(cond))

    async def _notify(self, cond: asyncio.Condition) -> None:
        async with cond:
            cond.notify_all()

    def condition(self) -> asyncio.Condition:
        if self._cond is None:
            self._cond = asyncio.Condition()
        return self._cond

    def as_dict(self) -> dict:
        return {"index": self.index, "paused": self.paused,
                "gen": self.gen, "n": self.n}

    # --- the things a button does -------------------------------------
    def toggle_pause(self) -> None:
        self.paused = not self.paused
        self.dwell = time.monotonic()
        self._bump()

    def step(self, by: int) -> None:
        if self.n > 0:
            self.index = (self.index + by) % self.n
        self.dwell = time.monotonic()
        self._bump()

    def set_cards(self, n: int) -> None:
        """A new payload. Keep the reading you were on where possible."""
        self.n = n
        self.index = self.index % n if n else 0
        self._bump()

    def tick(self) -> None:
        if self.paused or self.n <= 1:
            return
        if time.monotonic() - self.dwell >= CARD_S:
            self.step(1)


control = Control()


def _cards(payload: dict, provider: str, now: float) -> list[dict]:
    """Every quota one provider is metering, in the order it should be shown.

    One card per bar, each carrying its own label, because a percentage with
    no name on a 72x16 screen is just a number -- the device's own UI has the
    same problem and solves it with the pill.
    """
    out: list[dict] = []

    def add(label: str, pct, resets_in, kind: str) -> None:
        if pct is None:
            return
        # The device formats the countdown by WINDOW, not by label: a 5-hour
        # window is worth minutes, a 7-day one is not, and "Weekly" plus
        # "23h59m" does not fit the caption row anyway.
        card = {"provider": provider, "label": label, "pct": float(pct),
                "kind": kind}
        if isinstance(resets_in, int) and resets_in >= 0:
            # Seconds remaining at the moment of this poll, not an absolute
            # instant. The app ages it with its own elapsed time, so the
            # countdown never depends on the bar's clock being right.
            card["in_s"] = resets_in * 60
        out.append(card)

    session_label = payload.get("sm") or "Current"
    weekly_label = payload.get("wm") or "Weekly"
    if payload.get("has_s", True):
        # A provider that meters only one window sends it in the session slot;
        # "sk" says when that slot is really a week, so the countdown is not
        # timed to the minute for something seven days long.
        add(session_label, payload.get("s"), payload.get("sr"),
            payload.get("sk") or "session")
    if payload.get("has_w", True):
        add(weekly_label, payload.get("w"), payload.get("wr"), "weekly")

    # Scoped model allowances share the weekly reset instant.
    for scoped in payload.get("ws") or []:
        if isinstance(scoped, dict) and scoped.get("n"):
            add(scoped["n"], scoped.get("p"), payload.get("wr"), "weekly")

    # Reset credits are a count, not a proportion, so they ride as their own
    # shape rather than being forced into a bar.
    held, used = payload.get("rc"), payload.get("ru")
    if held or used:
        card = {"provider": provider, "label": "Resets", "kind": "credits",
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
    control.set_cards(len(cards))


def _query(target: str) -> dict[str, str]:
    _, _, qs = target.partition("?")
    out: dict[str, str] = {}
    for part in qs.split("&"):
        if "=" in part:
            k, _, v = part.partition("=")
            out[k] = v
    return out


async def _handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    try:
        request = await asyncio.wait_for(reader.readline(), timeout=5)
        parts = request.decode("latin-1").split()
        target = parts[1] if len(parts) >= 2 else ""
        path = target.split("?")[0]
        ok = len(parts) >= 2 and parts[0] == "GET" and path == PATH

        # A button, from whoever can see one. The in-process reader uses the
        # Control object directly; this is for a reader running outside the
        # daemon, which on macOS is the fallback when the daemon cannot reach
        # the bar's LAN address -- a launchd job cannot raise the Local
        # Network prompt, so it is denied silently. See
        # tools/busybar_lan_access.py for the fix, and this for when you would
        # rather not apply it.
        if len(parts) >= 2 and path == CONTROL_PATH:
            action = _query(target).get("do", "")
            if action == "pause":
                control.toggle_pause()
            elif action == "next":
                control.step(1)
            elif action == "prev":
                control.step(-1)
            body = json.dumps(control.as_dict()).encode()
            writer.write(
                f"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n"
                .encode() + body)
            await writer.drain()
            return

        if ok:
            q = _query(parts[1])
            since = q.get("since")
            wait_ms = min(int(q.get("wait") or 0), MAX_WAIT_MS)
            # Long poll: hold the request open until something changes, so a
            # button press reaches the screen in one round trip instead of
            # waiting out a polling interval. A timeout is not a failure --
            # answering anyway also lets the app redraw a canvas that
            # something else cleared.
            if since is not None and wait_ms > 0 and since == str(control.gen):
                cond = control.condition()
                try:
                    async with cond:
                        await asyncio.wait_for(
                            cond.wait_for(lambda: str(control.gen) != since),
                            timeout=wait_ms / 1000)
                except asyncio.TimeoutError:
                    pass
            body = json.dumps({**_latest, "control": control.as_dict()}).encode()
        else:
            body = b'{"error":"not found"}'

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


async def _rotate() -> None:
    """The rotation clock. Host-side, because the buttons that pause it are."""
    while True:
        await asyncio.sleep(TICK_S)
        control.tick()


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
    asyncio.get_running_loop().create_task(_rotate())
    log(f"busybar serve: {bind}:{port}{PATH}")
    return True


async def stop() -> None:
    global _server
    if _server is not None:
        _server.close()
        await _server.wait_closed()
        _server = None
