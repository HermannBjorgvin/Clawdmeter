#!/usr/bin/env python3
"""Tests for the conditional weekly Fable (scoped-model) field ("f").

The Fable percent comes from the OAuth usage endpoint's limits[] array
(kind "weekly_scoped" with a model scope), NOT from the /v1/messages
rate-limit headers — see fetch_fable_pct's docstring. The contract under
test is the omit-when-absent gate:

  - allowance present  -> payload carries "f":<0-100>
  - allowance absent   -> "f" key omitted entirely (no 0, no null)
  - Enterprise account -> usage endpoint never queried, "f" omitted
  - endpoint failure   -> "f" omitted (never blocks the main payload)

Both Python daemons (macOS + Windows) are exercised with the same cases.
All tests mock httpx so no real network calls are made.

Run: python -m pytest daemon/tests/test_fable.py -x -q
"""
import asyncio
import time
from unittest.mock import AsyncMock, MagicMock, patch

import httpx
import pytest

from daemon import claude_usage_daemon as mac_daemon
from daemon import claude_usage_daemon_windows as win_daemon

DAEMONS = [
    pytest.param(mac_daemon, id="macos"),
    pytest.param(win_daemon, id="windows"),
]


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _run(coro):
    """Run a coroutine on a private event loop (order-independent in the suite)."""
    loop = asyncio.new_event_loop()
    try:
        return loop.run_until_complete(coro)
    finally:
        loop.close()


def _mock_response(status_code=200, headers=None, json_data=None):
    resp = MagicMock()
    resp.status_code = status_code
    resp.text = "mocked"
    header_data = {k.lower(): v for k, v in (headers or {}).items()}
    resp.headers = MagicMock()
    resp.headers.get = lambda name, default=None: header_data.get(name.lower(), default)
    resp.json = MagicMock(return_value=json_data)
    return resp


def _mock_client(post_resp=None, get_resp=None, get_exc=None):
    client = AsyncMock()
    client.__aenter__ = AsyncMock(return_value=client)
    client.__aexit__ = AsyncMock(return_value=False)
    if post_resp is not None:
        client.post = AsyncMock(return_value=post_resp)
    if get_exc is not None:
        client.get = AsyncMock(side_effect=get_exc)
    else:
        client.get = AsyncMock(return_value=get_resp)
    return client


def _pro_headers(now):
    return {
        "anthropic-ratelimit-unified-5h-utilization": "0.30",
        "anthropic-ratelimit-unified-5h-reset": str(now + 3600),
        "anthropic-ratelimit-unified-7d-utilization": "0.90",
        "anthropic-ratelimit-unified-7d-reset": str(now + 86400),
        "anthropic-ratelimit-unified-5h-status": "allowed",
    }


def _ent_headers(now):
    return {
        "anthropic-ratelimit-unified-overage-utilization": "0.25",
        "anthropic-ratelimit-unified-overage-reset": str(now + 86400),
        "anthropic-ratelimit-unified-status": "allowed",
    }


def _usage_json(with_fable=True, percent=71):
    """Realistic /api/oauth/usage body (captured 2026-08-03, trimmed)."""
    limits = [
        {"kind": "session", "group": "session", "percent": 30,
         "severity": "normal", "scope": None, "is_active": False},
        {"kind": "weekly_all", "group": "weekly", "percent": 90,
         "severity": "critical", "scope": None, "is_active": True},
    ]
    if with_fable:
        limits.append({
            "kind": "weekly_scoped", "group": "weekly", "percent": percent,
            "severity": "normal",
            "scope": {"model": {"id": None, "display_name": "Fable"}, "surface": None},
            "is_active": False,
        })
    return {"limits": limits}


# ---------------------------------------------------------------------------
# poll_api integration: the omit-when-absent gate
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("daemon", DAEMONS)
def test_fable_present_adds_f_key(daemon):
    """An account with a weekly Fable allowance gets "f" in the payload."""
    now = time.time()
    client = _mock_client(
        post_resp=_mock_response(headers=_pro_headers(now)),
        get_resp=_mock_response(json_data=_usage_json(with_fable=True, percent=71)),
    )
    with patch.object(daemon.httpx, "AsyncClient", return_value=client):
        payload = _run(daemon.poll_api("fake-token"))
    assert payload is not None
    assert payload["f"] == 71
    # The rest of the payload is unchanged by the new field
    assert payload["s"] == 30
    assert payload["w"] == 90
    assert payload["acct"] == "pro"


@pytest.mark.parametrize("daemon", DAEMONS)
def test_no_fable_allowance_omits_f_key(daemon):
    """No weekly_scoped entry -> "f" absent entirely (not 0, not null)."""
    now = time.time()
    client = _mock_client(
        post_resp=_mock_response(headers=_pro_headers(now)),
        get_resp=_mock_response(json_data=_usage_json(with_fable=False)),
    )
    with patch.object(daemon.httpx, "AsyncClient", return_value=client):
        payload = _run(daemon.poll_api("fake-token"))
    assert payload is not None
    assert "f" not in payload


@pytest.mark.parametrize("daemon", DAEMONS)
def test_enterprise_never_queries_usage_endpoint(daemon):
    """Enterprise has no weekly window at all — no "f", and no extra request."""
    now = time.time()
    client = _mock_client(
        post_resp=_mock_response(headers=_ent_headers(now)),
        get_resp=_mock_response(json_data=_usage_json(with_fable=True)),
    )
    with patch.object(daemon.httpx, "AsyncClient", return_value=client):
        payload = _run(daemon.poll_api("fake-token"))
    assert payload is not None
    assert payload["acct"] == "ent"
    assert "f" not in payload
    client.get.assert_not_called()


@pytest.mark.parametrize("daemon", DAEMONS)
@pytest.mark.parametrize("failure", ["http_500", "network", "bad_json"])
def test_usage_endpoint_failure_omits_f_but_keeps_payload(daemon, failure):
    """A broken usage endpoint must never take down the main payload."""
    now = time.time()
    if failure == "http_500":
        kwargs = {"get_resp": _mock_response(status_code=500)}
    elif failure == "network":
        kwargs = {"get_exc": httpx.ConnectError("Connection refused")}
    else:  # bad_json
        bad = _mock_response()
        bad.json = MagicMock(side_effect=ValueError("not json"))
        kwargs = {"get_resp": bad}
    client = _mock_client(post_resp=_mock_response(headers=_pro_headers(now)), **kwargs)
    with patch.object(daemon.httpx, "AsyncClient", return_value=client):
        payload = _run(daemon.poll_api("fake-token"))
    assert payload is not None
    assert payload["s"] == 30
    assert "f" not in payload


# ---------------------------------------------------------------------------
# fetch_fable_pct unit cases
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("daemon", DAEMONS)
def test_fetch_fable_pct_zero_is_a_legitimate_value(daemon):
    """0% used is a real reading — must return 0, not None (sentinel is
    key-absence, never 0)."""
    client = _mock_client(get_resp=_mock_response(json_data=_usage_json(percent=0)))
    with patch.object(daemon.httpx, "AsyncClient", return_value=client):
        assert _run(daemon.fetch_fable_pct("fake-token")) == 0


@pytest.mark.parametrize("daemon", DAEMONS)
def test_fetch_fable_pct_clamps_to_0_100(daemon):
    client = _mock_client(get_resp=_mock_response(json_data=_usage_json(percent=140)))
    with patch.object(daemon.httpx, "AsyncClient", return_value=client):
        assert _run(daemon.fetch_fable_pct("fake-token")) == 100


@pytest.mark.parametrize("daemon", DAEMONS)
def test_fetch_fable_pct_requires_model_scope(daemon):
    """A weekly_scoped entry without a model scope (e.g. a future
    surface-scoped limit) must not be mistaken for the Fable allowance."""
    body = {"limits": [{"kind": "weekly_scoped", "group": "weekly", "percent": 40,
                        "scope": {"model": None, "surface": "cowork"}}]}
    client = _mock_client(get_resp=_mock_response(json_data=body))
    with patch.object(daemon.httpx, "AsyncClient", return_value=client):
        assert _run(daemon.fetch_fable_pct("fake-token")) is None


@pytest.mark.parametrize("daemon", DAEMONS)
def test_fetch_fable_pct_malformed_body_returns_none(daemon):
    for body in (None, [], {}, {"limits": None}, {"limits": ["nope"]}):
        client = _mock_client(get_resp=_mock_response(json_data=body))
        with patch.object(daemon.httpx, "AsyncClient", return_value=client):
            assert _run(daemon.fetch_fable_pct("fake-token")) is None, f"body={body!r}"


# ---------------------------------------------------------------------------
# Wire shape: "f" rides in the same compact JSON, well under BLE_BUF_SIZE
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("daemon", DAEMONS)
def test_wire_shape_with_fable(daemon):
    import json
    now = time.time()
    client = _mock_client(
        post_resp=_mock_response(headers=_pro_headers(now)),
        get_resp=_mock_response(json_data=_usage_json(percent=71)),
    )
    with patch.object(daemon.httpx, "AsyncClient", return_value=client):
        payload = _run(daemon.poll_api("fake-token"))
    wire = json.dumps(payload, separators=(",", ":"))
    assert '"f":71' in wire
    assert len(wire.encode()) < 512  # firmware BLE_BUF_SIZE
