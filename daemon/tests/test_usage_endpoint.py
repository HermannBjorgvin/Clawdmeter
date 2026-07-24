#!/usr/bin/env python3
"""Unit tests for poll_usage_endpoint (macOS + Windows daemons).

Covers the token-free /api/oauth/usage polling path: nominal Pro/Max
payloads with per-model scoped weekly limits ("m"/"mn"), fallback signaling
(None) for non-Pro/Max shapes and errors, the 429 cooldown, the no-AuthError
contract on 401/403 (poll_api stays the sole authority on token validity),
and malformed resets_at handling. Parametrized over both Python daemons so
the two implementations can't drift apart.

All tests mock httpx so no real network calls are made.

Run: python -m pytest daemon/tests/test_usage_endpoint.py -x -q
"""
import asyncio
import copy
import time
from unittest.mock import AsyncMock, MagicMock, patch

import pytest

import daemon.claude_usage_daemon as mac_daemon
import daemon.claude_usage_daemon_windows as win_daemon

DAEMONS = [
    pytest.param(mac_daemon, id="macos"),
    pytest.param(win_daemon, id="windows"),
]


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _usage_body(now: float) -> dict:
    """A realistic /api/oauth/usage response body (Pro/Max shape, one scoped
    per-model weekly limit), shaped after a live capture."""
    iso_5h = _iso(now + 3600)     # 60 minutes out
    iso_7d = _iso(now + 86400)    # 1440 minutes out
    return {
        "five_hour": {"utilization": 42.0, "resets_at": iso_5h},
        "seven_day": {"utilization": 10.0, "resets_at": iso_7d},
        "limits": [
            {"kind": "session", "group": "session", "percent": 42,
             "resets_at": iso_5h, "scope": None},
            {"kind": "weekly_all", "group": "weekly", "percent": 10,
             "resets_at": iso_7d, "scope": None},
            {"kind": "weekly_scoped", "group": "weekly", "percent": 66,
             "resets_at": iso_7d,
             "scope": {"model": {"id": None, "display_name": "Fable"},
                       "surface": None}},
        ],
    }


def _iso(epoch: float) -> str:
    return (
        time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(epoch)) + "+00:00"
    )


def _make_mock_response(status_code=200, body=None, text="mocked"):
    resp = MagicMock()
    resp.status_code = status_code
    resp.text = text
    if body is not None:
        resp.json = MagicMock(return_value=body)
    else:
        resp.json = MagicMock(side_effect=ValueError("not json"))
    return resp


def _patch_get(mock_resp):
    async def fake_get(*args, **kwargs):
        return mock_resp

    mock_client = AsyncMock()
    mock_client.__aenter__ = AsyncMock(return_value=mock_client)
    mock_client.__aexit__ = AsyncMock(return_value=False)
    mock_client.get = fake_get
    return patch("httpx.AsyncClient", return_value=mock_client)


def _run(coro):
    """Fresh event loop per call — keeps tests order-independent (see
    test_windows_poll.py for the 3.12 get_event_loop rationale)."""
    loop = asyncio.new_event_loop()
    try:
        return loop.run_until_complete(coro)
    finally:
        loop.close()


@pytest.fixture(autouse=True)
def _reset_cooldown():
    """The 429 cooldown is module-level state; clear it around every test."""
    mac_daemon._usage_endpoint_cooldown_until = 0.0
    win_daemon._usage_endpoint_cooldown_until = 0.0
    yield
    mac_daemon._usage_endpoint_cooldown_until = 0.0
    win_daemon._usage_endpoint_cooldown_until = 0.0


# ---------------------------------------------------------------------------
# Nominal path
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("mod", DAEMONS)
def test_nominal_payload_with_scoped_model(mod):
    now = time.time()
    resp = _make_mock_response(200, _usage_body(now))
    with _patch_get(resp):
        payload = _run(mod.poll_usage_endpoint("fake-token"))

    assert payload is not None
    assert payload["s"] == 42
    assert payload["w"] == 10
    assert payload["st"] == "allowed"
    assert payload["acct"] == "pro"
    assert payload["ok"] is True
    assert payload["m"] == 66
    assert payload["mn"] == "Fable"
    assert abs(payload["sr"] - 60) <= 1
    assert abs(payload["wr"] - 1440) <= 1


@pytest.mark.parametrize("mod", DAEMONS)
def test_model_name_truncated_to_15_chars(mod):
    body = _usage_body(time.time())
    body["limits"][2]["scope"]["model"]["display_name"] = "A" * 40
    with _patch_get(_make_mock_response(200, body)):
        payload = _run(mod.poll_usage_endpoint("fake-token"))
    assert payload is not None
    assert payload["mn"] == "A" * 15   # fits firmware's char model_name[16]


@pytest.mark.parametrize("mod", DAEMONS)
def test_first_named_scoped_limit_wins(mod):
    body = _usage_body(time.time())
    nameless = copy.deepcopy(body["limits"][2])
    nameless["scope"]["model"]["display_name"] = None
    second = copy.deepcopy(body["limits"][2])
    second["percent"] = 90
    second["scope"]["model"]["display_name"] = "Opus"
    body["limits"] = body["limits"][:2] + [nameless, body["limits"][2], second]
    with _patch_get(_make_mock_response(200, body)):
        payload = _run(mod.poll_usage_endpoint("fake-token"))
    assert payload is not None
    assert payload["m"] == 66          # nameless entry skipped, first named wins
    assert payload["mn"] == "Fable"


@pytest.mark.parametrize("mod", DAEMONS)
def test_no_scoped_limit_omits_model_fields(mod):
    body = _usage_body(time.time())
    body["limits"] = body["limits"][:2]
    with _patch_get(_make_mock_response(200, body)):
        payload = _run(mod.poll_usage_endpoint("fake-token"))
    assert payload is not None
    assert "m" not in payload          # firmware hides the row when absent
    assert "mn" not in payload


# ---------------------------------------------------------------------------
# Fallback signaling — every non-Pro/Max outcome must return None so the
# caller falls back to poll_api (which owns Enterprise detection and, on
# Windows, the AuthError toast contract).
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("mod", DAEMONS)
@pytest.mark.parametrize("missing", ["five_hour", "seven_day"])
def test_missing_bucket_falls_back(mod, missing):
    body = _usage_body(time.time())
    body[missing] = None               # e.g. Enterprise shape
    with _patch_get(_make_mock_response(200, body)):
        assert _run(mod.poll_usage_endpoint("fake-token")) is None


@pytest.mark.parametrize("mod", DAEMONS)
@pytest.mark.parametrize("status", [401, 403, 500, 503])
def test_http_errors_fall_back_without_raising(mod, status):
    with _patch_get(_make_mock_response(status, None, text="err")):
        assert _run(mod.poll_usage_endpoint("fake-token")) is None


@pytest.mark.parametrize("mod", DAEMONS)
def test_non_json_body_falls_back(mod):
    with _patch_get(_make_mock_response(200, None)):
        assert _run(mod.poll_usage_endpoint("fake-token")) is None


# ---------------------------------------------------------------------------
# 429 cooldown — a rate-limited endpoint must not be retried every cycle
# (regression guard for the PR #29 / #37 rate-limit incident).
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("mod", DAEMONS)
def test_429_sets_cooldown_and_skips_next_poll(mod):
    with _patch_get(_make_mock_response(429, None, text="rate limited")):
        assert _run(mod.poll_usage_endpoint("fake-token")) is None
    assert mod._usage_endpoint_cooldown_until > time.time()

    # Next poll inside the cooldown must not touch the network at all.
    async def explode(*args, **kwargs):
        raise AssertionError("endpoint polled during cooldown")

    mock_client = AsyncMock()
    mock_client.__aenter__ = AsyncMock(return_value=mock_client)
    mock_client.__aexit__ = AsyncMock(return_value=False)
    mock_client.get = explode
    with patch("httpx.AsyncClient", return_value=mock_client):
        assert _run(mod.poll_usage_endpoint("fake-token")) is None


@pytest.mark.parametrize("mod", DAEMONS)
def test_cooldown_expires(mod):
    mod._usage_endpoint_cooldown_until = time.time() - 1   # already expired
    resp = _make_mock_response(200, _usage_body(time.time()))
    with _patch_get(resp):
        assert _run(mod.poll_usage_endpoint("fake-token")) is not None


# ---------------------------------------------------------------------------
# resets_at robustness — garbage must degrade to 0, never raise
# (same guard class as _billing_period_info; see PRs #104 / #106).
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("mod", DAEMONS)
@pytest.mark.parametrize("bad_iso", [
    "not-a-date",
    "99999-01-01T00:00:00+00:00",       # out of range for fromisoformat
    "",
    None,
])
def test_malformed_resets_at_degrades_to_zero(mod, bad_iso):
    body = _usage_body(time.time())
    body["five_hour"]["resets_at"] = bad_iso
    body["seven_day"]["resets_at"] = bad_iso
    with _patch_get(_make_mock_response(200, body)):
        payload = _run(mod.poll_usage_endpoint("fake-token"))
    assert payload is not None
    assert payload["sr"] == 0
    assert payload["wr"] == 0


@pytest.mark.parametrize("mod", DAEMONS)
def test_z_suffix_resets_at_parses(mod):
    now = time.time()
    body = _usage_body(now)
    body["five_hour"]["resets_at"] = (
        time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(now + 3600)) + "Z"
    )
    with _patch_get(_make_mock_response(200, body)):
        payload = _run(mod.poll_usage_endpoint("fake-token"))
    assert payload is not None
    assert abs(payload["sr"] - 60) <= 1
