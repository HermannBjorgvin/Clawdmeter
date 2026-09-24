#!/usr/bin/env python3
"""Tests for the shared Anthropic header parser.

This logic used to be duplicated verbatim in the macOS and Windows daemons,
and the copies had drifted: Windows hardened _billing_period_info after a
field report (#104) where a garbage reset header raised OSError and killed the
poll loop; macOS never got the fix. The hardened version is now shared, so the
regression tests below cover both daemons at once.

Run: python -m pytest daemon/tests/test_claude_collector.py -x -q
"""
import time

from daemon.collectors import WINDOW_5H, WINDOW_7D
from daemon.collectors.claude import (
    _billing_period_info,
    payload_from_headers,
    snapshot_from_headers,
)

NOW = 1_787_000_000.0

PRO_HEADERS = {
    "anthropic-ratelimit-unified-5h-utilization": "0.42",
    "anthropic-ratelimit-unified-5h-reset": str(NOW + 3600),
    "anthropic-ratelimit-unified-7d-utilization": "0.15",
    "anthropic-ratelimit-unified-7d-reset": str(NOW + 86400),
    "anthropic-ratelimit-unified-5h-status": "allowed",
}
ENT_HEADERS = {
    "anthropic-ratelimit-unified-overage-utilization": "0.60",
    "anthropic-ratelimit-unified-overage-reset": str(NOW + 86400 * 5),
    "anthropic-ratelimit-unified-status": "allowed",
}


# --- wire payload -----------------------------------------------------------

def test_pro_account_reports_both_windows():
    p = payload_from_headers(PRO_HEADERS, NOW)
    assert (p["acct"], p["s"], p["w"], p["sr"], p["ok"]) == ("pro", 42, 15, 60, True)


def test_enterprise_account_reports_one_window_plus_billing_period():
    p = payload_from_headers(ENT_HEADERS, NOW)
    assert p["acct"] == "ent"
    assert (p["s"], p["w"], p["wr"]) == (60, 0, 0)
    assert 0 <= p["tp"] <= 100 and p["pd"] > 0


def test_absent_headers_degrade_to_enterprise_zeroes():
    """A 200 carrying no rate-limit headers at all must not throw."""
    p = payload_from_headers({}, NOW)
    assert (p["acct"], p["s"], p["tp"], p["pd"], p["rd"]) == ("ent", 0, 0, 30, "")


def test_garbage_utilization_reads_as_zero():
    p = payload_from_headers({**PRO_HEADERS,
                              "anthropic-ratelimit-unified-5h-utilization": "wat"}, NOW)
    # Non-empty but unparseable: still the Pro shape, just a zeroed number.
    assert p["acct"] == "pro" and p["s"] == 0


def test_elapsed_reset_reports_zero_not_negative():
    p = payload_from_headers({**PRO_HEADERS,
                              "anthropic-ratelimit-unified-5h-reset": str(NOW - 600)}, NOW)
    assert p["sr"] == 0


# --- #104 regression: garbage reset timestamps must never crash the loop -----

def test_far_future_reset_does_not_raise():
    """fromtimestamp() overflows on this; it used to kill the macOS poll loop."""
    assert _billing_period_info(NOW, "99999999999999") == {"tp": 0, "pd": 30, "rd": ""}


def test_zero_reset_does_not_step_back_into_1969():
    assert _billing_period_info(NOW, "0") == {"tp": 0, "pd": 30, "rd": ""}


def test_non_numeric_reset_degrades():
    assert _billing_period_info(NOW, "not-a-timestamp") == {"tp": 0, "pd": 30, "rd": ""}


# --- normalized snapshot ----------------------------------------------------

def test_pro_snapshot_carries_both_windows():
    snap = snapshot_from_headers(PRO_HEADERS, NOW)
    assert snap.provider == "claude" and snap.plan == "pro" and snap.live
    assert snap.windows[WINDOW_5H].used_percent == 42
    assert snap.windows[WINDOW_7D].used_percent == 15


def test_enterprise_snapshot_has_no_weekly_window():
    """Enterprise meters one spending limit; there is no 7-day bar to draw."""
    snap = snapshot_from_headers(ENT_HEADERS, NOW)
    assert set(snap.windows) == {WINDOW_5H}
    assert snap.plan == "ent"


def test_snapshot_shape_matches_the_codex_collector():
    """The point of the abstraction: the daemon cannot tell the two apart."""
    from daemon.collectors.codex import _windows_from_endpoint
    from daemon.collectors import UsageSnapshot

    claude = snapshot_from_headers(PRO_HEADERS, NOW)
    codex = UsageSnapshot(
        provider="codex", plan="pro",
        windows=_windows_from_endpoint({
            "primary_window": {"used_percent": 78, "limit_window_seconds": 604800,
                               "reset_after_seconds": 428901}}),
        source="oauth:wham/usage")

    for snap in (claude, codex):
        assert isinstance(snap.provider, str)
        assert all(0 <= w.used_percent <= 100 for w in snap.windows.values())
        assert set(snap.windows) <= {WINDOW_5H, WINDOW_7D}
