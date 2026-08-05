#!/usr/bin/env python3
"""Tests for the conditional weekly scoped-model limits field ("ws").

Scoped weekly percents (today only Fable; historically Opus/Sonnet had their
own buckets) come from the OAuth usage endpoint's limits[] array (kind
"weekly_scoped" with a model scope), NOT from the /v1/messages rate-limit
headers — see fetch_weekly_limits's docstring. The contract under test is the
omit-when-absent gate and the labeled-array shape:

  - scoped limits present -> payload carries "ws":[{"n":<label>,"p":<0-100>},...]
  - none                  -> "ws" key omitted entirely (no [], no null)
  - Enterprise account    -> usage endpoint never queried, "ws" omitted
  - endpoint failure      -> "ws" omitted (never blocks the main payload)

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


def _scoped_entry(name="Fable", percent=71):
    return {
        "kind": "weekly_scoped", "group": "weekly", "percent": percent,
        "severity": "normal",
        "scope": {"model": {"id": None, "display_name": name}, "surface": None},
        "is_active": False,
    }


def _usage_json(scoped=({"name": "Fable", "percent": 71},)):
    """Realistic /api/oauth/usage body (captured 2026-08-03, trimmed)."""
    limits = [
        {"kind": "session", "group": "session", "percent": 30,
         "severity": "normal", "scope": None, "is_active": False},
        {"kind": "weekly_all", "group": "weekly", "percent": 90,
         "severity": "critical", "scope": None, "is_active": True},
    ]
    for s in scoped:
        limits.append(_scoped_entry(s["name"], s["percent"]))
    return {"limits": limits}


# ---------------------------------------------------------------------------
# poll_api integration: the omit-when-absent gate + labeled-array shape
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("daemon", DAEMONS)
def test_scoped_present_adds_ws_key(daemon):
    """An account with a weekly Fable allowance gets a labeled ws entry."""
    now = time.time()
    client = _mock_client(
        post_resp=_mock_response(headers=_pro_headers(now)),
        get_resp=_mock_response(json_data=_usage_json()),
    )
    with patch.object(daemon.httpx, "AsyncClient", return_value=client):
        payload = _run(daemon.poll_api("fake-token"))
    assert payload is not None
    assert payload["ws"] == [{"n": "Fable", "p": 71}]
    # The rest of the payload is unchanged by the new field
    assert payload["s"] == 30
    assert payload["w"] == 90
    assert payload["acct"] == "pro"


@pytest.mark.parametrize("daemon", DAEMONS)
def test_multiple_scoped_models_all_ride_along(daemon):
    """A returning per-model bucket (e.g. Sonnet) is sent alongside Fable, in
    API order, each with its own label."""
    now = time.time()
    body = _usage_json(scoped=(
        {"name": "Fable", "percent": 71},
        {"name": "Sonnet", "percent": 40},
    ))
    client = _mock_client(
        post_resp=_mock_response(headers=_pro_headers(now)),
        get_resp=_mock_response(json_data=body),
    )
    with patch.object(daemon.httpx, "AsyncClient", return_value=client):
        payload = _run(daemon.poll_api("fake-token"))
    assert payload["ws"] == [{"n": "Fable", "p": 71}, {"n": "Sonnet", "p": 40}]


@pytest.mark.parametrize("daemon", DAEMONS)
def test_no_scoped_limits_omits_ws_key(daemon):
    """No weekly_scoped entries -> "ws" absent entirely (not [], not null)."""
    now = time.time()
    client = _mock_client(
        post_resp=_mock_response(headers=_pro_headers(now)),
        get_resp=_mock_response(json_data=_usage_json(scoped=())),
    )
    with patch.object(daemon.httpx, "AsyncClient", return_value=client):
        payload = _run(daemon.poll_api("fake-token"))
    assert payload is not None
    assert "ws" not in payload


@pytest.mark.parametrize("daemon", DAEMONS)
def test_enterprise_never_queries_usage_endpoint(daemon):
    """Enterprise has no weekly window at all — no "ws", and no extra request."""
    now = time.time()
    client = _mock_client(
        post_resp=_mock_response(headers=_ent_headers(now)),
        get_resp=_mock_response(json_data=_usage_json()),
    )
    with patch.object(daemon.httpx, "AsyncClient", return_value=client):
        payload = _run(daemon.poll_api("fake-token"))
    assert payload is not None
    assert payload["acct"] == "ent"
    assert "ws" not in payload
    client.get.assert_not_called()


@pytest.mark.parametrize("daemon", DAEMONS)
@pytest.mark.parametrize("failure", ["http_500", "network", "bad_json"])
def test_usage_endpoint_failure_omits_ws_but_keeps_payload(daemon, failure):
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
    assert "ws" not in payload


# ---------------------------------------------------------------------------
# fetch_weekly_limits unit cases
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("daemon", DAEMONS)
def test_zero_percent_is_a_legitimate_value(daemon):
    """0% used is a real reading — must be sent, not dropped (the firmware's
    "off" sentinel is key-absence, never 0)."""
    body = _usage_json(scoped=({"name": "Fable", "percent": 0},))
    client = _mock_client(get_resp=_mock_response(json_data=body))
    with patch.object(daemon.httpx, "AsyncClient", return_value=client):
        assert _run(daemon.fetch_weekly_limits("fake-token"))["scoped"] == [{"n": "Fable", "p": 0}]


@pytest.mark.parametrize("daemon", DAEMONS)
def test_percent_clamps_to_0_100(daemon):
    body = _usage_json(scoped=({"name": "Fable", "percent": 140},))
    client = _mock_client(get_resp=_mock_response(json_data=body))
    with patch.object(daemon.httpx, "AsyncClient", return_value=client):
        assert _run(daemon.fetch_weekly_limits("fake-token"))["scoped"] == [{"n": "Fable", "p": 100}]


@pytest.mark.parametrize("daemon", DAEMONS)
def test_requires_model_scope(daemon):
    """A weekly_scoped entry without a model scope (e.g. a future
    surface-scoped limit) must not be sent as a model bucket."""
    body = {"limits": [{"kind": "weekly_scoped", "group": "weekly", "percent": 40,
                        "scope": {"model": None, "surface": "cowork"}}]}
    client = _mock_client(get_resp=_mock_response(json_data=body))
    with patch.object(daemon.httpx, "AsyncClient", return_value=client):
        assert _run(daemon.fetch_weekly_limits("fake-token"))["scoped"] == []


@pytest.mark.parametrize("daemon", DAEMONS)
def test_label_falls_back_to_model_id(daemon):
    """A null display_name falls back to the model id; no usable name -> the
    entry is skipped (a bar the user can't identify is noise)."""
    body = {"limits": [
        {"kind": "weekly_scoped", "group": "weekly", "percent": 40,
         "scope": {"model": {"id": "claude-sonnet-5", "display_name": None}}},
        {"kind": "weekly_scoped", "group": "weekly", "percent": 20,
         "scope": {"model": {"id": None, "display_name": None}}},
    ]}
    client = _mock_client(get_resp=_mock_response(json_data=body))
    with patch.object(daemon.httpx, "AsyncClient", return_value=client):
        assert _run(daemon.fetch_weekly_limits("fake-token"))["scoped"] == [
            {"n": "claude-sonnet-5", "p": 40}
        ]


@pytest.mark.parametrize("daemon", DAEMONS)
def test_malformed_body_returns_none(daemon):
    for body in (None, [], {}, {"limits": None}, {"limits": ["nope"]}):
        client = _mock_client(get_resp=_mock_response(json_data=body))
        with patch.object(daemon.httpx, "AsyncClient", return_value=client):
            got = _run(daemon.fetch_weekly_limits("fake-token"))
            assert got is None or got["scoped"] == [], f"body={body!r}"


# ---------------------------------------------------------------------------
# Wire shape: "ws" rides in the same compact JSON, well under BLE_BUF_SIZE
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("daemon", DAEMONS)
def test_wire_shape_with_scoped_limits(daemon):
    import json
    now = time.time()
    body = _usage_json(scoped=(
        {"name": "Fable", "percent": 71},
        {"name": "Sonnet", "percent": 40},
    ))
    client = _mock_client(
        post_resp=_mock_response(headers=_pro_headers(now)),
        get_resp=_mock_response(json_data=body),
    )
    with patch.object(daemon.httpx, "AsyncClient", return_value=client):
        payload = _run(daemon.poll_api("fake-token"))
    wire = json.dumps(payload, separators=(",", ":"))
    assert '"ws":[{"n":"Fable","p":71},{"n":"Sonnet","p":40}]' in wire
    assert len(wire.encode()) < 512  # firmware BLE_BUF_SIZE


# ---------------------------------------------------------------------------
# Single-source rule: both weekly numbers share the endpoint's rounding
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("daemon", DAEMONS)
def test_weekly_all_rebased_on_endpoint_when_scoped_present(daemon):
    """The header quantizes to 2 decimals ("0.12") and the endpoint to a
    rounded integer, so a true 12.6/12.4 pair reads as 12/12 from the header
    but 13/12 from the endpoint. With a scoped limit on screen beside it, "w"
    must come from the endpoint too — otherwise the two faces of one card are
    quantized differently and can disagree with the settings UI."""
    now = time.time()
    headers = dict(_pro_headers(now))
    headers["anthropic-ratelimit-unified-7d-utilization"] = "0.12"  # -> 12 via the header
    body = _usage_json(scoped=({"name": "Fable", "percent": 12},))
    body["limits"][1]["percent"] = 13                               # weekly_all -> 13
    client = _mock_client(
        post_resp=_mock_response(headers=headers),
        get_resp=_mock_response(json_data=body),
    )
    with patch.object(daemon.httpx, "AsyncClient", return_value=client):
        payload = _run(daemon.poll_api("fake-token"))
    assert payload["w"] == 13, "all-models % must come from the endpoint, not the header"
    assert payload["ws"] == [{"n": "Fable", "p": 12}]


@pytest.mark.parametrize("daemon", DAEMONS)
def test_weekly_all_keeps_header_value_without_scoped_limits(daemon):
    """No scoped limit -> nothing to be inconsistent with, so the long-standing
    header-derived "w" is left exactly as it was (no behavior change for plans
    without a Fable allowance)."""
    now = time.time()
    headers = dict(_pro_headers(now))
    headers["anthropic-ratelimit-unified-7d-utilization"] = "0.12"
    body = _usage_json(scoped=())
    body["limits"][1]["percent"] = 13
    client = _mock_client(
        post_resp=_mock_response(headers=headers),
        get_resp=_mock_response(json_data=body),
    )
    with patch.object(daemon.httpx, "AsyncClient", return_value=client):
        payload = _run(daemon.poll_api("fake-token"))
    assert payload["w"] == 12
    assert "ws" not in payload


@pytest.mark.parametrize("daemon", DAEMONS)
def test_weekly_all_falls_back_to_header_when_endpoint_fails(daemon):
    now = time.time()
    headers = dict(_pro_headers(now))
    headers["anthropic-ratelimit-unified-7d-utilization"] = "0.12"
    client = _mock_client(
        post_resp=_mock_response(headers=headers),
        get_exc=httpx.ConnectError("Connection refused"),
    )
    with patch.object(daemon.httpx, "AsyncClient", return_value=client):
        payload = _run(daemon.poll_api("fake-token"))
    assert payload["w"] == 12
    assert "ws" not in payload
