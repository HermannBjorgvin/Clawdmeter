"""BUSY Bar as a secondary display (https://busy.app).

A 72x16 RGB LED matrix with an open HTTP API, reachable over USB or Wi-Fi with
no cloud round trip. One `POST /busybar/display/draw` carries a list of
elements -- text, rectangles, a countdown -- so the usage view ports over as
data rather than as pixels.

Three things about the device shape the layout:

  * **72x16 is the whole canvas.** One quota fits, not two. The 5-hour session
    window is the one that actually stops you, so that is what it shows; the
    weekly number is on the desk meter a few inches away.
  * **`countdown` is rendered on-device.** Hand it a Unix timestamp and the bar
    counts down by itself, so "resets in" stays true between polls instead of
    needing the heartbeat the BLE path needs.
  * **Text is printable ASCII only** (`^[\\x20-\\x7E]+$` in the API spec), the
    same constraint the firmware's subset fonts impose. `_ascii()` enforces it
    rather than letting the bar 400 on a stray character.

PRIORITY. Draws are accepted when their priority is >= the running app's, and
an active BUSY/CUSTOM focus session sits at 90. This publishes at 50, below
that, so a focus session preempts the meter -- correct, since the point of the
device is telling people you are busy, and a usage widget must not stomp it.
The cost is that the meter is absent exactly while you are heads-down; raising
the number would "fix" that by breaking the device's actual job.
"""

from __future__ import annotations

import re
import time

import httpx

APP_NAME = "petmeter"

# THE DEVICE AND THE CLOUD MOUNT THE SAME API AT DIFFERENT PREFIXES. The
# published OpenAPI spec at api.busy.app documents `/busybar/...`, which is the
# cloud relay's namespace; the bar itself serves `/api/...` (blog.busy.app's
# widget guide, confirmed against hardware). Posting to the spec's path hits
# the device's web-UI file server instead, which answers `405 Allow: GET` --
# an error that reads like "wrong method" and is really "wrong prefix".
DRAW_PATH = "/api/display/draw"
ACCESS_PATH = "/api/access"
PRIORITY = 50                  # below a BUSY session's 90 -- see module docstring
# The bar answers a draw in ~5.0s, measured repeatedly over Wi-Fi, and stops
# answering entirely for ~20s stretches after a burst of requests. A timeout
# set at the response time is not a safety margin, it is a coin flip -- the
# first live attempt died on a 5s ConnectTimeout against a device that was
# about to answer. This does not block the poll loop (see sinks/__init__.py),
# so it can afford to wait.
HTTP_TIMEOUT = 20.0

WIDTH, HEIGHT = 72, 16         # front panel, in pixels
BAR_Y, BAR_H = 13, 3           # a thin rule along the bottom edge

# Same thresholds and meanings as the firmware's pct_color().
WARN_PCT, CRIT_PCT = 75.0, 90.0
COL_OK = "#8FA76BFF"           # THEME_GREEN
COL_WARN = "#D97757FF"         # THEME_AMBER
COL_CRIT = "#C0392BFF"         # THEME_RED
COL_TEXT = "#FAF9F5FF"
COL_TRACK = "#2A2A28FF"

_PRINTABLE = re.compile(r"[^\x20-\x7E]")


def _ascii(text: str) -> str:
    return _PRINTABLE.sub("", text)


def _color(pct: float) -> str:
    if pct >= CRIT_PCT:
        return COL_CRIT
    if pct >= WARN_PCT:
        return COL_WARN
    return COL_OK


def elements_for(payload: dict, now: float | None = None) -> dict:
    """The draw request for one usage payload.

    Separate from the transport so the layout can be tested, and eyeballed as
    JSON, without a device on the desk.
    """
    now = time.time() if now is None else now
    pct = float(payload.get("s") or 0.0)
    resets_in = payload.get("sr")
    has_data = bool(payload.get("ok")) and payload.get("has_s", True)

    body: dict = {"application_name": APP_NAME, "priority": PRIORITY, "elements": []}

    if not has_data:
        body["elements"].append({
            "id": "pct", "type": "text", "text": "no data",
            "font": "small", "color": COL_TRACK, "x": 0, "y": 3,
            "align": "top_left", "display": "front", "timeout": 0,
        })
        return body

    body["elements"].append({
        "id": "pct", "type": "text", "text": _ascii(f"{pct:.0f}%"),
        "font": "normal", "color": COL_TEXT, "x": 0, "y": 2,
        "align": "top_left", "display": "front", "timeout": 0,
    })

    # The reset instant, counted down by the device itself. Omitted rather than
    # faked when the daemon has no window -- a frozen countdown is worse than
    # none, and this is the number people act on.
    if isinstance(resets_in, int) and resets_in >= 0:
        body["elements"].append({
            "id": "reset", "type": "countdown",
            "timestamp": str(int(now + resets_in * 60)),
            "direction": "time_left", "show_hours": "when_non_zero",
            "color": COL_TRACK, "x": 34, "y": 2,
            "align": "top_left", "display": "front", "timeout": 0,
        })

    body["elements"].append({
        "id": "track", "type": "rectangle", "x": 0, "y": BAR_Y,
        "width": WIDTH, "height": BAR_H, "radius": 1,
        "color": COL_TRACK, "display": "front", "timeout": 0,
    })
    filled = max(1, round(WIDTH * min(pct, 100.0) / 100.0)) if pct > 0 else 0
    if filled:
        body["elements"].append({
            "id": "fill", "type": "rectangle", "x": 0, "y": BAR_Y,
            "width": filled, "height": BAR_H, "radius": 1,
            "color": _color(pct), "display": "front", "timeout": 0,
        })

    # The status LED only blinks where a glance at the bar would not be enough.
    if pct >= CRIT_PCT:
        body["led_notification_color"] = COL_CRIT
    return body


class BusyBarSink:
    """Pushes the active provider's session quota to a BUSY Bar."""

    name = "busybar"

    def __init__(self, base_url: str, token: str = "") -> None:
        self.base_url = base_url.rstrip("/")
        self.token = token

    async def show(self, payload: dict) -> bool:
        headers = {"Authorization": f"Bearer {self.token}"} if self.token else {}
        async with httpx.AsyncClient(timeout=HTTP_TIMEOUT) as client:
            resp = await client.post(self.base_url + DRAW_PATH,
                                     json=elements_for(payload), headers=headers)
        # 409 is the device saying a higher-priority app owns the screen -- a
        # focus session, usually. Expected, not a failure.
        if resp.status_code == 409:
            return True
        return resp.status_code == 200


def from_config() -> BusyBarSink | None:
    """A sink if `busybar_url` is configured, else None."""
    try:                              # both invocation styles, as in the daemon
        from ..claude_usage_daemon import read_config_value
    except ImportError:               # pragma: no cover
        from claude_usage_daemon import read_config_value

    url = read_config_value("busybar_url", allowed=None)
    if not url:
        return None
    return BusyBarSink(url, read_config_value("busybar_token", allowed=None))


if __name__ == "__main__":                    # pragma: no cover
    # Dry run: `python -m daemon.sinks.busybar` prints the draw request for a
    # sample payload; add a URL to actually send it.
    #
    #   python -m daemon.sinks.busybar
    #   python -m daemon.sinks.busybar http://busybar.local
    #
    # A 404 from every /busybar/... path means the device's HTTP API is off --
    # enable it in the BUSY Bar settings (it is password/key gated) and retry.
    import asyncio
    import json as _json
    import sys

    sample = {"s": 34.0, "sr": 281, "ok": True, "has_s": True}
    print(_json.dumps(elements_for(sample), indent=2))
    if len(sys.argv) <= 1:
        sys.exit(0)

    url = sys.argv[1]
    token = sys.argv[2] if len(sys.argv) > 2 else ""

    async def _probe() -> int:
        headers = {"Authorization": f"Bearer {token}"} if token else {}
        async with httpx.AsyncClient(timeout=HTTP_TIMEOUT) as client:
            resp = await client.post(url.rstrip("/") + DRAW_PATH,
                                     json=elements_for(sample), headers=headers)
        print(f"HTTP {resp.status_code} in {resp.elapsed.total_seconds():.1f}s")
        if resp.status_code == 200:
            print("drawn")
        elif resp.status_code == 409:
            print("a higher-priority app owns the screen (a focus session?) -- "
                  "expected, and not a failure")
        elif resp.status_code in (401, 403):
            # Ask the device rather than inferring. GET /api/access is itself
            # ungated and answers {"mode": disabled|enabled|key, "key_valid":}.
            try:
                async with httpx.AsyncClient(timeout=HTTP_TIMEOUT) as c2:
                    access = (await c2.get(url.rstrip("/") + ACCESS_PATH)).json()
            except (httpx.HTTPError, ValueError):
                access = {}
            mode = access.get("mode", "?")
            print(f"access denied; the device reports mode={mode!r}")
            if mode == "disabled":
                print("Turn on HTTP API access over Wi-Fi in the BUSY Bar "
                      "settings ('enabled' for open on your LAN, or 'key' with "
                      "a 4-10 digit key you then pass as the second argument). "
                      "Over USB the bar is 10.0.4.20 and this Wi-Fi gate does "
                      "not apply.")
            elif mode == "key":
                print("It is in key mode -- pass the 4-10 digit key as the "
                      "second argument.")
        elif resp.status_code == 405 and resp.headers.get("Allow") == "GET":
            print(f"{url} answered from its web UI, not its API -- check the "
                  f"path prefix (the device serves /api/..., not /busybar/...).")
        else:
            print(resp.text[:300])
        return 0 if resp.status_code in (200, 409) else 1

    try:
        sys.exit(asyncio.run(_probe()))
    except httpx.TimeoutException:
        # Worth naming: the bar goes unresponsive for ~20s stretches after a
        # burst, which looks exactly like it has fallen off the network.
        print(f"no answer within {HTTP_TIMEOUT:.0f}s. The bar goes quiet for "
              "~20s after a burst of requests -- wait, then retry once.")
        sys.exit(1)
    except httpx.HTTPError as exc:
        print(f"{type(exc).__name__}: {exc}")
        sys.exit(1)
