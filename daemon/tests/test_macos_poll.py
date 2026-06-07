#!/usr/bin/env python3
"""Unit tests for the macOS daemon's poll_api — overage-mode rate-limit handling.

Mirrors the Windows daemon's poll tests for the shared unified rate-limit logic.
In overage mode the API omits the -5h-/-7d- breakdown (representative-claim ==
"overage"); the daemon must report the spent windows as full instead of zeros.
Ref: https://github.com/anthropics/claude-code/issues/12829

Run: python -m pytest daemon/tests/test_macos_poll.py -q
"""
import asyncio
import time
from unittest.mock import AsyncMock, MagicMock, patch

from daemon.claude_usage_daemon import poll_api


def _run(coro):
    return asyncio.run(coro)


def _poll_with_headers(headers, status_code=200):
    resp = MagicMock()
    resp.status_code = status_code
    resp.text = "mocked"
    resp.headers = MagicMock()
    resp.headers.get = lambda name, default=None: headers.get(name.lower(), default)

    client = AsyncMock()
    client.__aenter__ = AsyncMock(return_value=client)
    client.__aexit__ = AsyncMock(return_value=False)
    client.post = AsyncMock(return_value=resp)
    with patch("httpx.AsyncClient", return_value=client):
        return _run(poll_api("fake-token"))


def test_normal_mode_reads_5h_and_7d_utilization():
    now = time.time()
    payload = _poll_with_headers({
        "anthropic-ratelimit-unified-status": "allowed",
        "anthropic-ratelimit-unified-5h-utilization": "0.42",
        "anthropic-ratelimit-unified-5h-reset": str(now + 3600),
        "anthropic-ratelimit-unified-7d-utilization": "0.10",
        "anthropic-ratelimit-unified-7d-reset": str(now + 86400),
    })
    assert payload["s"] == 42
    assert payload["w"] == 10
    assert payload["st"] == "allowed"
    assert abs(payload["sr"] - 60) <= 1
    assert abs(payload["wr"] - 1440) <= 1


def test_overage_mode_reports_full_subscription_and_overage_status():
    now = time.time()
    payload = _poll_with_headers({
        "anthropic-ratelimit-unified-status": "allowed",
        "anthropic-ratelimit-unified-representative-claim": "overage",
        "anthropic-ratelimit-unified-overage-in-use": "true",
        "anthropic-ratelimit-unified-overage-utilization": "0.0",
        "anthropic-ratelimit-unified-reset": str(now + 3600),
    })
    assert payload["s"] == 100
    assert payload["w"] == 100
    assert payload["st"] == "overage"
    assert abs(payload["sr"] - 60) <= 1
    assert abs(payload["wr"] - 60) <= 1


def test_overage_payload_carries_real_overage_utilization():
    """The "o" field exposes the true overage spend (for the device's "Extra
    Usage" bar), distinct from the faked 100% plan bar."""
    now = time.time()
    payload = _poll_with_headers({
        "anthropic-ratelimit-unified-status": "allowed",
        "anthropic-ratelimit-unified-representative-claim": "overage",
        "anthropic-ratelimit-unified-overage-in-use": "true",
        "anthropic-ratelimit-unified-overage-utilization": "0.23",
        "anthropic-ratelimit-unified-reset": str(now + 3600),
    })
    assert payload["st"] == "overage"
    assert payload["s"] == 100
    assert payload["o"] == 23
    assert abs(payload["or"] - 60) <= 1


def test_normal_mode_reports_zero_overage():
    """Outside overage the extra-usage figure is 0 (no phantom Extra Usage bar)."""
    now = time.time()
    payload = _poll_with_headers({
        "anthropic-ratelimit-unified-status": "allowed",
        "anthropic-ratelimit-unified-overage-utilization": "0.0",
        "anthropic-ratelimit-unified-5h-utilization": "0.30",
        "anthropic-ratelimit-unified-5h-reset": str(now + 3600),
        "anthropic-ratelimit-unified-7d-utilization": "0.74",
        "anthropic-ratelimit-unified-7d-reset": str(now + 86400),
    })
    assert payload["o"] == 0
    assert payload["st"] == "allowed"


def test_status_prefers_unified_over_per_window():
    now = time.time()
    payload = _poll_with_headers({
        "anthropic-ratelimit-unified-status": "rate_limited",
        "anthropic-ratelimit-unified-5h-status": "allowed",
        "anthropic-ratelimit-unified-5h-utilization": "1.0",
        "anthropic-ratelimit-unified-5h-reset": str(now + 3600),
        "anthropic-ratelimit-unified-7d-utilization": "0.5",
        "anthropic-ratelimit-unified-7d-reset": str(now + 86400),
    })
    assert payload["st"] == "rate_limited"
    assert payload["s"] == 100
    assert payload["w"] == 50


def test_missing_headers_default_to_zero_unknown():
    payload = _poll_with_headers({})
    assert payload["s"] == 0
    assert payload["w"] == 0
    assert payload["st"] == "unknown"
    assert payload["ok"] is True


# Account-type discriminator ("acct") + overage-in-use flag ("oiu") — mirrors the
# Windows daemon's POLL-ACCT-01 (DEBT.md D-3): 5h/7d present -> "pro-max";
# windowless overage -> "ent"; neither -> "pro-max" (default).

def test_acct_pro_max_when_subscription_windows_present():
    now = time.time()
    payload = _poll_with_headers({
        "anthropic-ratelimit-unified-5h-utilization": "0.30",
        "anthropic-ratelimit-unified-5h-reset": str(now + 3600),
        "anthropic-ratelimit-unified-7d-utilization": "0.10",
        "anthropic-ratelimit-unified-7d-reset": str(now + 86400),
        "anthropic-ratelimit-unified-status": "allowed",
    })
    assert payload["acct"] == "pro-max"
    assert payload["oiu"] is False


def test_oiu_true_with_windows_present_stays_pro_max():
    now = time.time()
    payload = _poll_with_headers({
        "anthropic-ratelimit-unified-5h-utilization": "1.0",
        "anthropic-ratelimit-unified-5h-status": "rejected",
        "anthropic-ratelimit-unified-5h-reset": str(now + 3600),
        "anthropic-ratelimit-unified-7d-utilization": "0.11",
        "anthropic-ratelimit-unified-7d-reset": str(now + 86400),
        "anthropic-ratelimit-unified-overage-in-use": "true",
        "anthropic-ratelimit-unified-representative-claim": "five_hour",
        "anthropic-ratelimit-unified-status": "rejected",
    })
    assert payload["acct"] == "pro-max"
    assert payload["oiu"] is True
    assert payload["s"] == 100


def test_acct_ent_when_windowless_overage():
    now = time.time()
    payload = _poll_with_headers({
        "anthropic-ratelimit-unified-status": "allowed",
        "anthropic-ratelimit-unified-representative-claim": "overage",
        "anthropic-ratelimit-unified-overage-in-use": "true",
        "anthropic-ratelimit-unified-overage-utilization": "0.54",
        "anthropic-ratelimit-unified-reset": str(now + 3600),
    })
    assert payload["acct"] == "ent"
    assert payload["oiu"] is True


def test_acct_defaults_pro_max_when_no_recognisable_headers():
    payload = _poll_with_headers({})
    assert payload["acct"] == "pro-max"
    assert payload["oiu"] is False
