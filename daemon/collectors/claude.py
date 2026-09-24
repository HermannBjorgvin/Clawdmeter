"""Claude (Anthropic) usage collection.

Anthropic reports usage as response headers on any API call, so the daemon
fires a 1-token request and reads `anthropic-ratelimit-unified-*` off the
response. There is no usage endpoint; the headers ride along free.

This module owns the header parsing only. Transport stays in each daemon,
which differ in ways that are not incidental -- the macOS daemon raises
TokenExpired and the Windows one raises AuthError to drive a "run claude
login" toast. Both previously carried byte-identical copies of the parsing
below, which had already drifted: the Windows `_billing_period_info` was
hardened after a field report (#104) where a garbage reset header raised
OSError(22) and killed the poll loop, and the macOS copy never got the fix.
The hardened version is the one kept here.

Two account shapes come back:

  * Pro/Max     -- 5h and 7d windows, `unified-5h-*` / `unified-7d-*`
  * Enterprise  -- a single spending-limit window, `unified-overage-*`, plus a
                   derived billing-period fraction

Free-ride credential rule: a 401/403 means "no data", never a refresh. Claude
Code owns that token -- see daemon/tests/test_freeride.py.
"""

from __future__ import annotations

import calendar
import datetime
import time
from typing import Mapping

from . import UsageSnapshot, Window, WINDOW_5H, WINDOW_7D

PRO_5H_UTIL = "anthropic-ratelimit-unified-5h-utilization"

# The probe request. Anthropic has no usage endpoint -- the rate-limit headers
# come back on any call, so the cheapest possible one is the probe.
API_URL = "https://api.anthropic.com/v1/messages"
API_HEADERS_TEMPLATE = {
    "anthropic-version": "2023-06-01",
    "anthropic-beta": "oauth-2025-04-20",
    "Content-Type": "application/json",
    "User-Agent": "claude-code/2.1.5",
}
API_BODY = {
    "model": "claude-haiku-4-5-20251001",
    "max_tokens": 1,
    "messages": [{"role": "user", "content": "hi"}],
}


def _billing_period_info(now: float, reset_ts: str) -> dict:
    """Fraction of billing period elapsed (tp, 0-100) and period length in days (pd).

    Billing periods are assumed calendar-monthly: period_end is the reset
    timestamp, period_start is the same day/time one calendar month earlier.

    The rate-limit headers expose only the reset timestamp, not the period
    length, so the monthly window is an assumption -- but a documented one:
    Enterprise spend-limit `period` "the only value today is monthly" (Claude
    Enterprise Admin API reference). The doc notes period is an open string
    that may gain other values later; revisit this if so.
    """
    try:
        period_end = float(reset_ts)
    except ValueError:
        return {"tp": 0, "pd": 30, "rd": ""}
    if period_end <= 0:
        # reset_ts defaults to "0" whenever the overage-reset header is absent
        # (e.g. a 200 that simply carries no billing headers). fromtimestamp(0)
        # is 1970; stepping one month back lands in 1969, and
        # datetime.timestamp() raises OSError for pre-1970 dates on Windows.
        return {"tp": 0, "pd": 30, "rd": ""}
    try:
        dt_end = datetime.datetime.fromtimestamp(period_end)
        prev_month = dt_end.month - 1 or 12
        prev_year = dt_end.year if dt_end.month > 1 else dt_end.year - 1
        prev_day = min(dt_end.day, calendar.monthrange(prev_year, prev_month)[1])
        dt_start = dt_end.replace(year=prev_year, month=prev_month, day=prev_day)
        period_start = dt_start.timestamp()
    except (OSError, OverflowError, ValueError):
        # Beyond the <= 0 guard above (#104): fromtimestamp()/timestamp() also
        # raise for out-of-range non-zero values (e.g. a far-future
        # "99999999999999" header). Garbage must never crash the poll loop.
        return {"tp": 0, "pd": 30, "rd": ""}

    period_len = period_end - period_start
    if period_len <= 0:
        return {"tp": 0, "pd": 30, "rd": ""}
    pct_val = (now - period_start) / period_len * 100
    return {
        "tp": max(0, min(100, int(round(pct_val)))),
        "pd": int(round(period_len / 86400)),
        "rd": f"{dt_end.strftime('%b')} {dt_end.day}",
    }


def _pct(util: str) -> int:
    """Header utilization (0.0-1.0) as a whole percent. Garbage reads as 0."""
    try:
        return int(round(float(util) * 100))
    except ValueError:
        return 0


def _reset_minutes(reset_ts: str, now: float) -> int:
    """Minutes until an absolute reset timestamp; 0 once it has passed."""
    try:
        r = float(reset_ts)
    except ValueError:
        return 0
    mins = (r - now) / 60.0
    return int(round(mins)) if mins > 0 else 0


def payload_from_headers(headers: Mapping[str, str], now: float | None = None) -> dict:
    """The terse BLE wire payload for one API response's headers.

    Device-side config (chime, clock) is added by the caller -- those are
    presentation settings read from a per-platform config file, not usage.
    """
    now = time.time() if now is None else now

    def hdr(name: str, default: str = "0") -> str:
        return headers.get(name, default)

    if headers.get(PRO_5H_UTIL):
        return {
            "s": _pct(hdr(PRO_5H_UTIL)),
            "sr": _reset_minutes(hdr("anthropic-ratelimit-unified-5h-reset"), now),
            "w": _pct(hdr("anthropic-ratelimit-unified-7d-utilization")),
            "wr": _reset_minutes(hdr("anthropic-ratelimit-unified-7d-reset"), now),
            "st": hdr("anthropic-ratelimit-unified-5h-status", "unknown"),
            "acct": "pro",
            "ok": True,
        }

    reset_ts = hdr("anthropic-ratelimit-unified-overage-reset")
    return {
        "s": _pct(hdr("anthropic-ratelimit-unified-overage-utilization")),
        "sr": _reset_minutes(reset_ts, now),
        "w": 0,
        "wr": 0,
        "st": hdr("anthropic-ratelimit-unified-status", "unknown"),
        "acct": "ent",
        **_billing_period_info(now, reset_ts),
        "ok": True,
    }


def snapshot_from_headers(headers: Mapping[str, str],
                          now: float | None = None) -> UsageSnapshot:
    """The provider-agnostic view of the same headers.

    Enterprise accounts have no 7-day window -- their single spending-limit
    window maps onto 5h so the device has one bar to render, and `plan`
    carries the distinction.
    """
    now = time.time() if now is None else now
    payload = payload_from_headers(headers, now)

    windows = {WINDOW_5H: Window(payload["s"], resets_in=payload["sr"] * 60)}
    if payload["acct"] == "pro":
        windows[WINDOW_7D] = Window(payload["w"], resets_in=payload["wr"] * 60)

    return UsageSnapshot(
        provider="claude",
        plan=payload["acct"],
        windows=windows,
        source="headers:api.anthropic.com",
        live=True,
        stale_seconds=0,
    )


class ClaudeCollector:
    """Collector protocol implementation for Anthropic.

    Token acquisition is injected rather than done here: the macOS daemon
    reads the Keychain, the Windows and Linux daemons read
    ~/.claude/.credentials.json, and none of that belongs in a parser.

    The daemons still call poll_api directly today -- they need to distinguish
    a dead token (TokenExpired / AuthError, which drives a "run claude login"
    toast) from a transient failure, whereas the protocol flattens both to
    None. They migrate to this once the multi-provider loop lands and can
    carry that distinction itself.
    """

    provider = "claude"

    def __init__(self, token_provider):
        self._token_provider = token_provider

    async def collect(self) -> UsageSnapshot | None:
        import httpx

        token = self._token_provider()
        if not token:
            return None

        headers = dict(API_HEADERS_TEMPLATE)
        headers["Authorization"] = f"Bearer {token}"
        try:
            async with httpx.AsyncClient(timeout=20.0) as http:
                resp = await http.post(API_URL, headers=headers, json=API_BODY)
        except httpx.HTTPError:
            return None
        # 401/403 included: a dead token is "no data", never a refresh.
        if resp.status_code >= 400:
            return None
        return snapshot_from_headers(resp.headers)
