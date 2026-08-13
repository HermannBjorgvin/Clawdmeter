"""OpenCode Go usage, for the device's OpenCode screen.

Two sources, both local-first:

1. **Limits** — `GET https://opencode.ai/zen/go/v1/usage` with the key OpenCode
   already stored in auth.json. Undocumented but stable-shaped, and it mirrors
   Anthropic's model exactly: rolling / weekly / monthly windows, each with a
   `percent` and an ISO `resetsAt`. That is what makes the OpenCode screen a
   real gauge instead of a lone number.

2. **Tokens** — a read-only stdlib sqlite3 query against OpenCode's own
   database (`$OPENCODE_DATA_DIR/opencode.db`, default ~/.local/share/opencode),
   whose `session` table carries per-session token counters and `model`. This is
   a decoration on the gauges, not the headline.

   NOTE: older OpenCode versions wrote storage/message/*.json instead, which is
   what third-party tools like ccusage parse. Current versions do not, so those
   tools silently report nothing here.

Free-ride, like the Anthropic side: we read the key OpenCode stored and never
refresh or rotate it. Every failure path returns None, the daemon omits the
fields, and the device shows "No data" — never a fabricated 0%.

urllib, not httpx: the bash daemon shells out to this module with `--fragment`
under whatever python3 the host has, which is not the daemons' venv.
"""

from __future__ import annotations

import json
import os
import sqlite3
import time
import urllib.error
import urllib.request
from datetime import date, datetime, timezone
from pathlib import Path

USAGE_URL = "https://opencode.ai/zen/go/v1/usage"
_TIMEOUT = 15
# Windows move in percent points over hours; the daemon polls every 60 s.
_CACHE_TTL = 60

_cache: tuple[float, dict | None] | None = None


def _data_dir() -> Path:
    data_dir = os.environ.get("OPENCODE_DATA_DIR")
    if not data_dir:
        xdg = os.environ.get("XDG_DATA_HOME")
        base = Path(xdg) if xdg else Path.home() / ".local" / "share"
        data_dir = str(base / "opencode")
    return Path(data_dir)


def _api_key() -> str | None:
    """The OpenCode Go key, from the env or OpenCode's own auth.json."""
    env = os.environ.get("OPENCODE_API_KEY")
    if env:
        return env
    try:
        with open(_data_dir() / "auth.json", encoding="utf-8") as fh:
            entry = json.load(fh).get("opencode-go")
    except (OSError, json.JSONDecodeError):
        return None
    if isinstance(entry, dict):
        key = entry.get("key")
        return key if isinstance(key, str) and key else None
    return None


def _reset_minutes(iso: str | None, now: float | None = None) -> int:
    """Minutes until an ISO-8601 `resetsAt`, floored at 0. -1 when unparseable,
    which the firmware renders as "---" rather than "resets now"."""
    if not iso:
        return -1
    try:
        # fromisoformat handles the trailing Z only from 3.11; normalise it.
        parsed = datetime.fromisoformat(iso.replace("Z", "+00:00"))
    except ValueError:
        return -1
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    now = time.time() if now is None else now
    mins = (parsed.timestamp() - now) / 60.0
    return int(round(mins)) if mins > 0 else 0


def _window(usage: dict, name: str) -> dict:
    w = usage.get(name)
    return w if isinstance(w, dict) else {}


def _pct(w: dict) -> int:
    try:
        return max(0, min(100, int(round(float(w.get("percent") or 0)))))
    except (TypeError, ValueError):
        return 0


def parse_usage(body: dict, now: float | None = None) -> dict | None:
    """Reduce the /usage response to the device payload fields.

    Rolling and weekly map onto the same gauge pair the Anthropic screen uses;
    monthly has no panel of its own and rides along as a number.
    """
    usage = body.get("usage")
    if not isinstance(usage, dict) or not usage:
        return None
    rolling, weekly, monthly = (_window(usage, k) for k in ("rolling", "weekly", "monthly"))
    return {
        "ocs": _pct(rolling),
        "ocsr": _reset_minutes(rolling.get("resetsAt"), now),
        "ocw": _pct(weekly),
        "ocwr": _reset_minutes(weekly.get("resetsAt"), now),
        "ocmo": _pct(monthly),
        "ocst": str(rolling.get("status") or "unknown")[:15],
    }


def _fetch_limits(key: str, log=None) -> dict | None:
    # The User-Agent is load-bearing: Cloudflare fronts opencode.ai and rejects
    # urllib's default "Python-urllib/3.x" with 403 error code 1010. Any normal
    # UA passes. (httpx sends its own, which is why it worked in testing.)
    req = urllib.request.Request(
        USAGE_URL,
        headers={"Authorization": f"Bearer {key}", "User-Agent": "clawdmeter-daemon/1.0"},
    )
    try:
        with urllib.request.urlopen(req, timeout=_TIMEOUT) as resp:
            body = json.loads(resp.read().decode("utf-8"))
    except (urllib.error.URLError, OSError, json.JSONDecodeError, ValueError) as e:
        # Never log the key, and never let a dead endpoint kill the Anthropic poll.
        if log:
            log(f"OpenCode usage endpoint failed: {type(e).__name__}")
        return None
    return parse_usage(body)


def _today_bounds_ms(now: float | None = None) -> tuple[int, int]:
    """[start, end) of the local day in epoch milliseconds — the unit
    `session.time_updated` uses."""
    today = date.today() if now is None else datetime.fromtimestamp(now).date()
    start = datetime(today.year, today.month, today.day).timestamp()
    return int(start * 1000), int((start + 86400) * 1000)


def _model_label(blob: str | None) -> str:
    """`model` is a JSON blob like {"id":"deepseek-v4-flash","providerID":…}."""
    if not blob:
        return ""
    try:
        parsed = json.loads(blob)
    except (json.JSONDecodeError, TypeError):
        return str(blob)[:19]
    if isinstance(parsed, dict):
        return str(parsed.get("id") or "")[:19]
    return str(parsed)[:19]


def summarize_sessions(rows) -> dict | None:
    """Today's token total and busiest model from `session` rows.

    ponytail: a session started yesterday and touched today counts its whole
    lifetime into today, since `session` only stores running totals. Walking the
    `part` rows would date each turn exactly; not worth it for a desk gauge.
    """
    total_tokens = 0
    top_model, top_model_tokens = "", -1
    seen = False
    for model, t_in, t_out, t_reason, t_cread, t_cwrite in rows:
        seen = True
        tokens = sum(int(v or 0) for v in (t_in, t_out, t_reason, t_cread, t_cwrite))
        total_tokens += tokens
        if tokens > top_model_tokens:
            top_model, top_model_tokens = _model_label(model), tokens
    if not seen:
        return None
    return {"oct": total_tokens, "ocm": top_model}


_QUERY = """
    SELECT model, tokens_input, tokens_output, tokens_reasoning,
           tokens_cache_read, tokens_cache_write
    FROM session
    WHERE time_updated >= ? AND time_updated < ?
"""


def _fetch_tokens(log=None) -> dict | None:
    db = _data_dir() / "opencode.db"
    if not db.exists():
        return None
    start_ms, end_ms = _today_bounds_ms()
    try:
        # Read-only URI so we never write to (or create) OpenCode's database,
        # and a short timeout so a busy WAL writer can't wedge the poll.
        uri = "file:" + db.as_posix() + "?mode=ro"
        with sqlite3.connect(uri, uri=True, timeout=2.0) as conn:
            return summarize_sessions(conn.execute(_QUERY, (start_ms, end_ms)))
    except sqlite3.Error as e:
        if log:
            log(f"OpenCode DB read failed: {e}")
        return None


def opencode_today(log=None) -> dict | None:
    """Payload fragment for the OpenCode screen, or None if unavailable.

    The limits are what the screen is for, so no limits means no fields at all —
    today's tokens ride along only when the gauges are there to decorate.
    """
    global _cache
    now = time.monotonic()
    if _cache and now - _cache[0] < _CACHE_TTL:
        return _cache[1]

    result = None
    key = _api_key()
    if key:
        result = _fetch_limits(key, log)
        if result:
            tokens = _fetch_tokens(log)
            if tokens:
                result.update(tokens)
    elif log:
        log("OpenCode API key not found (no auth.json entry, no OPENCODE_API_KEY)")

    _cache = (now, result)
    return result


def add_opencode_fields(payload: dict, log=None) -> None:
    """Merge the OpenCode screen's fields into an outgoing payload, if any."""
    fields = opencode_today(log)
    if fields:
        payload.update(fields)


def _fragment() -> str:
    """`,"ocs":…,"ocsr":…` for the bash daemon to splice into its awk printf, or
    an empty string. Same additive shape as its chime/clock fragments."""
    fields = opencode_today()
    if not fields:
        return ""
    return "".join(f",{json.dumps(k)}:{json.dumps(v)}" for k, v in fields.items())


if __name__ == "__main__":
    import sys

    if "--fragment" in sys.argv:
        print(_fragment(), end="")
        raise SystemExit(0)

    if "--show" in sys.argv:   # what the daemon would send right now
        print(opencode_today(log=print))
        raise SystemExit(0)

    # Self-checks for the two reductions. Both are pure, so no network here.
    now = datetime(2026, 8, 12, 20, 0, tzinfo=timezone.utc).timestamp()
    body = {
        "usage": {
            "rolling": {"status": "ok", "percent": 0, "resetsAt": "2026-08-13T03:36:05.687Z"},
            "weekly": {"status": "ok", "percent": 12, "resetsAt": "2026-08-17T00:00:00.687Z"},
            "monthly": {"status": "ok", "percent": 6, "resetsAt": "2026-09-12T14:07:15.687Z"},
        }
    }
    got = parse_usage(body, now)
    # 2026-08-13T03:36Z is 456 min out; 2026-08-17T00:00Z is 4d4h = 6000 min.
    assert got == {"ocs": 0, "ocsr": 456, "ocw": 12, "ocwr": 6000, "ocmo": 6, "ocst": "ok"}, got
    assert parse_usage({}, now) is None                     # not the shape we expect
    assert parse_usage({"usage": {}}, now) is None           # no windows at all
    # A window the API omits must read as 0% / unknown reset, never crash.
    assert parse_usage({"usage": {"rolling": {"percent": 5}}}, now) == {
        "ocs": 5, "ocsr": -1, "ocw": 0, "ocwr": -1, "ocmo": 0, "ocst": "unknown",
    }
    assert _reset_minutes(None) == -1 and _reset_minutes("garbage") == -1
    assert _reset_minutes("2026-08-12T19:00:00Z", now) == 0  # already past → 0, not negative
    assert _pct({"percent": 250}) == 100 and _pct({"percent": None}) == 0

    rows = [
        ('{"id":"grok-code-fast-1","providerID":"opencode-go"}', 30500, 2507, 0, 316800, 0),
        ('{"id":"small"}', 10, 5, 0, 0, 0),
    ]
    assert summarize_sessions(rows) == {"oct": 349822, "ocm": "grok-code-fast-1"}
    assert summarize_sessions([]) is None
    assert _model_label('{"providerID":"x"}') == "" and _model_label("raw") == "raw"
    print("ok")
