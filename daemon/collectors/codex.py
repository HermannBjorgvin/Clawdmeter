"""Codex usage collector.

Two sources, in preference order:

1. `GET https://chatgpt.com/backend-api/wham/usage` with the OAuth token the
   Codex CLI stores in ~/.codex/auth.json. Live, one request, and explicit
   about which limit it is reporting. This is what the Codex CLI and CodexBar
   use. It is UNDOCUMENTED and can change without notice, hence (2).

2. The `rate_limits` object the CLI records into its session rollout JSONL
   under ~/.codex/sessions. No network, but only as fresh as your last Codex
   session -- reported honestly via UsageSnapshot.stale_seconds.

ACCOUNT LIMIT vs PER-MODEL LIMITS -- the trap in this data. Two different
things are reported side by side:

  * the plan limit           (top-level `rate_limit`, log `limit_id` "codex")
  * per-model buckets, e.g.  GPT-5.3-Codex-Spark / metered_feature
                             "codex_bengalfox"

They are unrelated numbers. Taking the newest rollout record blindly read 0%
off a per-model bucket while the account was at 78%. Both readers below select
the account limit explicitly; per-model buckets are exposed separately by the
probe script but never merged into the snapshot.

Per the free-ride rule in this package's docstring, a 401 here means "no data",
never a token refresh -- the Codex CLI owns that token.
"""

from __future__ import annotations

import asyncio
import datetime
import json
import os
import time
import urllib.error
import urllib.request
from pathlib import Path

from . import UsageSnapshot, Window, WINDOW_5H, WINDOW_7D

USAGE_URL = "https://chatgpt.com/backend-api/wham/usage"
CREDITS_URL = "https://chatgpt.com/backend-api/wham/rate-limit-reset-credits"
HISTORY_URL = CREDITS_URL + "/history"
DEFAULT_CODEX_DIR = Path(os.path.expanduser("~/.codex"))
# The endpoint is slow even when healthy -- successful calls measured at 3-4s,
# and during a degraded spell 6 of 8 attempts exceeded 10s. A short timeout
# there does not fail fast, it just hands the poll to the stale-log fallback.
HTTP_TIMEOUT = 25.0

# The account-level bucket, as named in the rollout logs. Anything else
# ("codex_bengalfox", ...) is a per-model limit and must not be read as the
# plan limit.
ACCOUNT_LIMIT_ID = "codex"

# How old a rollout record may be and still stand in for a live reading. The
# fallback exists to ride out a brief endpoint outage, not to keep yesterday's
# number on screen: a 16-hour-old record reported 69% of a weekly window while
# the live endpoint said 10%, because usage moved on and the window shifted
# underneath it. Beyond this, report nothing and let the device say "no data" --
# a blank panel is recoverable, a confidently wrong number is not.
MAX_LOG_AGE_S = 3600

# limit_window_seconds (endpoint) and window_minutes (logs) -> window label.
WINDOW_BY_SECONDS = {18000: WINDOW_5H, 604800: WINDOW_7D}
WINDOW_BY_MINUTES = {300: WINDOW_5H, 10080: WINDOW_7D}


def _read_auth(codex_dir: Path) -> tuple[str, str | None] | None:
    """(access_token, account_id) from auth.json, or None if signed out."""
    try:
        tokens = json.loads((codex_dir / "auth.json").read_text()).get("tokens") or {}
    except (OSError, json.JSONDecodeError):
        return None
    token = tokens.get("access_token")
    return (token, tokens.get("account_id")) if token else None


# The usage endpoint reports how MANY reset credits are available but nothing
# about them. Two more endpoints fill the card in:
#
#   .../rate-limit-reset-credits          granted_at/expires_at per credit
#   .../rate-limit-reset-credits/history  granted and used events, inside a
#                                         server-defined trailing window
#                                         (window_start..as_of, 30 days today)
#
# Together they give the ledger the device draws: what you still hold, plus
# what you spent in the window, which is how many resets the window gave you
# to work with. A credit used inside the window was often granted before it,
# so "granted in window" does NOT reconcile with what you hold -- available
# plus used is the only arithmetic that does.
#
# Both are slow, and neither moves on its own, so they are cached for an hour.
_CREDITS_TTL_S = 3600
_credits_cache: dict[str, object] = {"at": 0.0, "stamps": (), "used": 0,
                                     "available": None}


def _credit_state(token: str, account_id: str | None,
                  available: int) -> tuple[tuple[tuple[float, float], ...], int]:
    """((granted, expires) per held credit, soonest first), credits spent.

    `available` is the live count from wham/usage, and a change in it busts the
    cache early: spending a credit moves it from one side of the ledger to the
    other, and letting the "available" side drop an hour before the "used" side
    ticks up would leave the card briefly disagreeing with itself.
    """
    now = time.time()
    fresh = now - float(_credits_cache["at"]) < _CREDITS_TTL_S
    if fresh and _credits_cache["available"] == available:
        return _credits_cache["stamps"], int(_credits_cache["used"])

    stamps = _fetch_stamps(token, account_id)
    if stamps is None:                       # endpoint down: keep what we had
        return _credits_cache["stamps"], int(_credits_cache["used"])
    used = _fetch_used(token, account_id)
    if used is None:
        used = int(_credits_cache["used"])

    _credits_cache.update(at=now, stamps=stamps, used=used, available=available)
    return stamps, used


def _get_json(url: str, token: str, account_id: str | None) -> dict | None:
    req = urllib.request.Request(url, method="GET")
    req.add_header("Authorization", f"Bearer {token}")
    req.add_header("Accept", "application/json")
    req.add_header("User-Agent", "petmeter")
    req.add_header("OpenAI-Beta", "codex-1")
    req.add_header("originator", "Codex Desktop")
    if account_id:
        req.add_header("ChatGPT-Account-Id", account_id)
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as resp:
            return json.loads(resp.read().decode())
    except (urllib.error.URLError, json.JSONDecodeError, TimeoutError):
        return None


def _fetch_stamps(token: str,
                  account_id: str | None) -> tuple[tuple[float, float], ...] | None:
    """(granted, expires) epochs per available credit, soonest expiry first."""
    raw = _get_json(CREDITS_URL, token, account_id)
    if raw is None:
        return None
    out = []
    for c in raw.get("credits") or []:
        if c.get("status") != "available":
            continue
        try:
            out.append((_iso_epoch(c["granted_at"]), _iso_epoch(c["expires_at"])))
        except (KeyError, TypeError, ValueError):
            continue
    out.sort(key=lambda pair: pair[1])
    return tuple(out)


def _fetch_used(token: str, account_id: str | None) -> int | None:
    """Credits spent inside the provider's own trailing window."""
    raw = _get_json(HISTORY_URL, token, account_id)
    if raw is None:
        return None
    return sum(1 for e in raw.get("events") or []
               if isinstance(e, dict) and e.get("kind") == "used")


def _iso_epoch(stamp: str) -> float:
    return datetime.datetime.fromisoformat(
        stamp.replace("Z", "+00:00")).timestamp()


def _windows_from_endpoint(rate_limit: dict) -> dict[str, Window]:
    """Both slots of an endpoint rate_limit block, keyed by window length.

    primary/secondary are positional, not semantic: a Pro plan limit puts its
    7-day window in primary with secondary null, while a per-model block uses
    primary for 5h and secondary for 7d. Always classify by duration.
    """
    out: dict[str, Window] = {}
    for slot in ("primary_window", "secondary_window"):
        w = rate_limit.get(slot)
        if not isinstance(w, dict):
            continue
        label = WINDOW_BY_SECONDS.get(w.get("limit_window_seconds"))
        if label is None:
            continue
        out[label] = Window(
            used_percent=float(w.get("used_percent") or 0.0),
            resets_in=w.get("reset_after_seconds"),
        )
    return out


def collect_via_oauth(codex_dir: Path = DEFAULT_CODEX_DIR) -> UsageSnapshot | None:
    """Live read. None on signed-out, dead token, or any network trouble."""
    auth = _read_auth(codex_dir)
    if auth is None:
        return None
    token, account_id = auth

    req = urllib.request.Request(USAGE_URL, method="GET")
    req.add_header("Authorization", f"Bearer {token}")
    req.add_header("Accept", "application/json")
    req.add_header("User-Agent", "petmeter")
    if account_id:
        req.add_header("ChatGPT-Account-Id", account_id)

    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as resp:
            raw = json.loads(resp.read().decode())
    except (urllib.error.URLError, json.JSONDecodeError, TimeoutError):
        # Includes HTTPError, so a 401 lands here as "no data" -- see the
        # free-ride rule. Never refresh the token.
        return None

    windows = _windows_from_endpoint(raw.get("rate_limit") or {})
    if not windows:
        return None

    model_windows = {}
    for entry in raw.get("additional_rate_limits") or []:
        if not isinstance(entry, dict):
            continue
        name = entry.get("limit_name") or entry.get("metered_feature")
        w = _windows_from_endpoint(entry.get("rate_limit") or {})
        if name and w:
            model_windows[name] = w

    credits = raw.get("rate_limit_reset_credits") or {}
    count = int(credits.get("available_count") or 0)
    stamps, used = _credit_state(token, account_id, count)

    return UsageSnapshot(
        provider="codex",
        plan=raw.get("plan_type"),
        windows=windows,
        model_windows=model_windows,
        reset_credits=count,
        reset_credits_used=used,
        reset_credits_expire=stamps[0][1] if stamps else None,
        source="oauth:wham/usage",
        live=True,
        stale_seconds=0,
    )


def _dig(obj, key):
    """Depth-first search for `key` anywhere in a decoded JSON value."""
    if isinstance(obj, dict):
        if key in obj:
            return obj[key]
        for v in obj.values():
            found = _dig(v, key)
            if found is not None:
                return found
    elif isinstance(obj, list):
        for v in obj:
            found = _dig(v, key)
            if found is not None:
                return found
    return None


def _account_rate_limits(path: Path) -> dict | None:
    """The last ACCOUNT-level rate_limits record in one rollout file."""
    try:
        lines = path.read_text(errors="replace").splitlines()
    except OSError:
        return None
    # Scan backwards -- newest record is last, and these files get large.
    for line in reversed(lines):
        if '"rate_limits"' not in line:
            continue
        try:
            rl = _dig(json.loads(line), "rate_limits")
        except json.JSONDecodeError:
            continue
        if isinstance(rl, dict) and rl.get("limit_id") == ACCOUNT_LIMIT_ID:
            return rl
    return None


def collect_via_logs(codex_dir: Path = DEFAULT_CODEX_DIR) -> UsageSnapshot | None:
    """Offline fallback. Only as fresh as the last Codex session."""
    files = []
    for root in (codex_dir / "sessions", codex_dir / "archived_sessions"):
        if root.is_dir():
            files.extend(root.rglob("rollout-*.jsonl"))
    files.sort(key=lambda p: p.stat().st_mtime, reverse=True)

    now = time.time()
    for path in files:
        if now - path.stat().st_mtime > MAX_LOG_AGE_S:
            break            # sorted newest-first: everything after is older
        rl = _account_rate_limits(path)
        if not rl:
            continue

        windows: dict[str, Window] = {}
        for slot in ("primary", "secondary"):
            w = rl.get(slot)
            if not isinstance(w, dict):
                continue
            label = WINDOW_BY_MINUTES.get(w.get("window_minutes"))
            if label is None:
                continue
            resets_at = w.get("resets_at")
            # A record whose window has already reset describes a window that
            # no longer exists. Its percentage is not merely stale, it is about
            # a different accounting period -- exactly the case that reported
            # 69% of a spent week while the live endpoint said 10% of a fresh
            # one. Drop it rather than carry it forward.
            if resets_at and resets_at <= now:
                continue
            windows[label] = Window(
                used_percent=float(w.get("used_percent") or 0.0),
                resets_in=int(resets_at - now) if resets_at else None,
            )
        if not windows:
            continue

        return UsageSnapshot(
            provider="codex",
            plan=rl.get("plan_type"),
            windows=windows,
            source=f"logs:{path.name}",
            live=False,
            stale_seconds=int(now - path.stat().st_mtime),
        )
    return None


class CodexCollector:
    """Live endpoint first, rollout logs as fallback."""

    provider = "codex"

    def __init__(self, codex_dir: Path = DEFAULT_CODEX_DIR):
        self.codex_dir = codex_dir

    async def collect(self) -> UsageSnapshot | None:
        # Both readers block -- urllib on the network, and the log fallback on
        # file IO across every rollout file. Off the event loop they go.
        return await asyncio.to_thread(self.collect_blocking)

    def collect_blocking(self) -> UsageSnapshot | None:
        """Synchronous variant, for callers already running off the loop."""
        return collect_via_oauth(self.codex_dir) or collect_via_logs(self.codex_dir)
