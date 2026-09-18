#!/usr/bin/env python3
"""Unit tests for the zero-token usage endpoint path — poll_usage_endpoint.

The daemon's primary usage source is GET /api/oauth/usage (no tokens billed,
strictly more data than the rate-limit headers). It is UNDOCUMENTED, so the
contract these tests pin down is as much about the fallback as the happy path:
anything unexpected must hand off to the header path inside poll_api rather
than guess.

All tests mock httpx — no real network calls are made.

Run: python -m pytest daemon/tests/test_usage_endpoint.py -x -q
"""
import asyncio
import datetime
import json
from unittest.mock import AsyncMock, MagicMock, patch

import pytest

import daemon.claude_usage_daemon as mod
from daemon.claude_usage_daemon import TokenExpired, poll_api, poll_usage_endpoint


# ---------------------------------------------------------------------------
# Fixtures / helpers
# ---------------------------------------------------------------------------

# A frozen clock. Everything below is expressed relative to it so the
# minutes-until maths is exact rather than "within a minute".
FROZEN_NOW_ISO = "2026-09-06T02:00:00+00:00"
FROZEN_NOW = datetime.datetime.fromisoformat(FROZEN_NOW_ISO).timestamp()


def _iso(minutes_from_now: float) -> str:
    """An ISO-8601 UTC instant `minutes_from_now` from the frozen clock."""
    dt = datetime.datetime.fromtimestamp(
        FROZEN_NOW + minutes_from_now * 60, datetime.timezone.utc
    )
    return dt.isoformat()


# The real response from a live Pro/Max account, verbatim apart from the two
# resets_at values, which are pinned to the frozen clock: five_hour resets in
# 500 minutes, seven_day in 4380. Keys we ignore are kept so the shape check
# is exercised against the actual payload, not a trimmed-down stand-in.
def _real_response() -> dict:
    return {
        "five_hour": {
            "utilization": 16.0,
            "resets_at": _iso(500),
            "limit_dollars": None,
            "used_dollars": None,
            "remaining_dollars": None,
            "locked_reason": None,
        },
        "seven_day": {
            "utilization": 34.0,
            "resets_at": _iso(4380),
            "limit_dollars": None,
            "used_dollars": None,
            "remaining_dollars": None,
            "locked_reason": None,
        },
        "seven_day_oauth_apps": None,
        "seven_day_opus": None,
        "seven_day_sonnet": None,
        "seven_day_cowork": None,
        "seven_day_omelette": None,
        "tangelo": None,
        "iguana_necktie": None,
        "omelette_promotional": None,
        "nimbus_quill": {"utilization": 0.0, "resets_at": None},
        "cinder_cove": None,
        "copper_kite": None,
        "amber_ladder": None,
        "juniper_tide": None,
        "extra_usage": {
            "is_enabled": False,
            "monthly_limit": None,
            "used_credits": None,
            "utilization": None,
            "currency": None,
            "decimal_places": None,
            "disabled_reason": None,
            "user_disabled": True,
            "spend_limit_reached": False,
            "credits_ever_enabled": True,
            "daily": None,
            "weekly": None,
        },
        "limits": [
            {"kind": "session", "group": "session", "percent": 16,
             "severity": "normal", "resets_at": _iso(500),
             "scope": None, "is_active": False},
            {"kind": "weekly_all", "group": "weekly", "percent": 34,
             "severity": "normal", "resets_at": _iso(4380),
             "scope": None, "is_active": True},
            {"kind": "weekly_scoped", "group": "weekly", "percent": 5,
             "severity": "normal", "resets_at": _iso(4380),
             "scope": {"model": {"id": None, "display_name": "Fable"},
                       "surface": None},
             "is_active": False},
        ],
        "spend": {
            "used": {"amount_minor": 0, "currency": "USD", "exponent": 2},
            "limit": None, "percent": 0, "severity": "normal",
            "enabled": False, "disabled_reason": None, "cap": None,
            "balance": None, "auto_reload": None, "disclaimer": "...",
            "can_purchase_credits": False, "can_toggle": False,
        },
        "member_dashboard_available": False,
    }


def _make_response(status_code=200, body=None, raw_text=None, headers=None):
    """A mock httpx.Response.

    `body` is returned by .json(); `raw_text` instead makes .json() raise the
    way httpx does on a non-JSON body.
    """
    resp = MagicMock()
    resp.status_code = status_code
    resp.text = raw_text if raw_text is not None else "mocked"
    if raw_text is not None:
        resp.json = MagicMock(
            side_effect=json.JSONDecodeError("Expecting value", raw_text, 0)
        )
    else:
        resp.json = MagicMock(return_value=body)
    header_data = {k.lower(): v for k, v in (headers or {}).items()}
    resp.headers = MagicMock()
    resp.headers.get = lambda name, default=None: header_data.get(name.lower(), default)
    return resp


# Header-path response used whenever a test asserts the fallback ran. Distinct
# values from the endpoint fixture so the two sources can never be confused.
def _header_response():
    return _make_response(
        status_code=200,
        headers={
            "anthropic-ratelimit-unified-5h-utilization": "0.77",
            "anthropic-ratelimit-unified-5h-reset": str(FROZEN_NOW + 600),   # 10 min
            "anthropic-ratelimit-unified-7d-utilization": "0.88",
            "anthropic-ratelimit-unified-7d-reset": str(FROZEN_NOW + 1200),  # 20 min
            "anthropic-ratelimit-unified-5h-status": "allowed",
        },
    )


class _Client:
    """Stands in for httpx.AsyncClient, recording every GET/POST it serves."""

    def __init__(self, get_response=None, post_response=None, get_error=None):
        self.get_response = get_response
        self.post_response = post_response
        self.get_error = get_error
        self.gets = []
        self.posts = []

    async def __aenter__(self):
        return self

    async def __aexit__(self, *exc):
        return False

    async def get(self, url, **kwargs):
        self.gets.append((url, kwargs))
        if self.get_error is not None:
            raise self.get_error
        return self.get_response

    async def post(self, url, **kwargs):
        self.posts.append((url, kwargs))
        return self.post_response


def _run(coro):
    """Run a coroutine on a private loop.

    A fresh loop per call keeps these tests order-independent in the full
    suite — see the same helper in test_windows_poll.py.
    """
    loop = asyncio.new_event_loop()
    try:
        return loop.run_until_complete(coro)
    finally:
        loop.close()


@pytest.fixture(autouse=True)
def _isolated(tmp_path, monkeypatch):
    """Freeze the clock, clear the 429 bench, and detach the real user config.

    Without the config detach, a developer's own `chime`/`clock` settings would
    leak extra keys into the payload and make equality assertions machine-
    dependent.
    """
    monkeypatch.setattr(mod, "CONFIG_FILE", tmp_path / "config")  # absent
    monkeypatch.setattr(mod, "_usage_endpoint_benched_until", 0.0)
    monkeypatch.setattr(mod.time, "time", lambda: FROZEN_NOW)
    yield


# ---------------------------------------------------------------------------
# Happy path: the real response parses to the right numbers
# ---------------------------------------------------------------------------

def test_real_response_parses_to_expected_payload():
    client = _Client(get_response=_make_response(body=_real_response()))
    with patch("httpx.AsyncClient", return_value=client):
        payload = _run(poll_usage_endpoint("tok"))

    assert payload == {
        "s": 16,
        "sr": 500,
        "w": 34,
        "wr": 4380,
        "st": "allowed",     # limits[kind=session].severity "normal"
        "acct": "pro",
        "ok": True,
    }
    assert client.gets and client.gets[0][0] == mod.USAGE_URL


def test_utilization_is_not_double_converted():
    """16.0 means 16 percent here, unlike the headers' 0..1 fraction."""
    client = _Client(get_response=_make_response(body=_real_response()))
    with patch("httpx.AsyncClient", return_value=client):
        payload = _run(poll_usage_endpoint("tok"))
    assert payload["s"] == 16, "utilization multiplied by 100 — 16.0 is already a %"
    assert payload["w"] == 34
    assert payload["s"] <= 100 and payload["w"] <= 100


def test_payload_keys_match_the_header_path_exactly():
    """The firmware parses these keys; the endpoint must add none of its own.

    Notably: the per-model weekly_scoped bucket in `limits` is deliberately
    NOT surfaced (Anthropic drops Fable's separate weekly limit 2026-09-14).
    """
    endpoint_client = _Client(get_response=_make_response(body=_real_response()))
    with patch("httpx.AsyncClient", return_value=endpoint_client):
        from_endpoint = _run(poll_usage_endpoint("tok"))

    header_client = _Client(get_response=_make_response(status_code=500),
                            post_response=_header_response())
    with patch("httpx.AsyncClient", return_value=header_client):
        from_headers = _run(poll_api("tok"))

    assert set(from_endpoint) == set(from_headers)


def test_endpoint_payload_wins_when_poll_api_composes_both():
    """poll_api prefers the endpoint and never spends the 1-token POST."""
    client = _Client(get_response=_make_response(body=_real_response()),
                     post_response=_header_response())
    with patch("httpx.AsyncClient", return_value=client):
        payload = _run(poll_api("tok"))
    assert payload["s"] == 16 and payload["sr"] == 500
    assert client.posts == [], "header path was called even though the endpoint answered"


def test_no_session_limit_entry_yields_unknown_status():
    body = _real_response()
    body["limits"] = [e for e in body["limits"] if e["kind"] != "session"]
    client = _Client(get_response=_make_response(body=body))
    with patch("httpx.AsyncClient", return_value=client):
        payload = _run(poll_usage_endpoint("tok"))
    assert payload["st"] == "unknown"   # same default the header path uses
    assert payload["s"] == 16           # ...and the numbers still come through


# ---------------------------------------------------------------------------
# Reset-time maths: rounding, and never negative
# ---------------------------------------------------------------------------

def test_elapsed_resets_at_yields_zero_not_negative():
    body = _real_response()
    body["five_hour"]["resets_at"] = _iso(-90)    # 90 minutes in the past
    body["seven_day"]["resets_at"] = _iso(-0.5)   # just past
    client = _Client(get_response=_make_response(body=body))
    with patch("httpx.AsyncClient", return_value=client):
        payload = _run(poll_usage_endpoint("tok"))
    assert payload["sr"] == 0
    assert payload["wr"] == 0


def test_null_resets_at_yields_zero():
    """Inactive buckets report resets_at: null; treat it like an absent header."""
    body = _real_response()
    body["five_hour"]["resets_at"] = None
    client = _Client(get_response=_make_response(body=body))
    with patch("httpx.AsyncClient", return_value=client):
        payload = _run(poll_usage_endpoint("tok"))
    assert payload["sr"] == 0


def test_sub_minute_resets_round_to_nearest_minute():
    body = _real_response()
    body["five_hour"]["resets_at"] = _iso(29.6)    # -> 30
    body["seven_day"]["resets_at"] = _iso(120.4)   # -> 120
    client = _Client(get_response=_make_response(body=body))
    with patch("httpx.AsyncClient", return_value=client):
        payload = _run(poll_usage_endpoint("tok"))
    assert payload["sr"] == 30
    assert payload["wr"] == 120


def test_naive_resets_at_is_read_as_utc_not_local():
    """No offset in the string must not skew the countdown by the host's tz."""
    naive = datetime.datetime.fromtimestamp(
        FROZEN_NOW + 60 * 60, datetime.timezone.utc
    ).replace(tzinfo=None).isoformat()
    body = _real_response()
    body["five_hour"]["resets_at"] = naive
    client = _Client(get_response=_make_response(body=body))
    with patch("httpx.AsyncClient", return_value=client):
        payload = _run(poll_usage_endpoint("tok"))
    assert payload["sr"] == 60


def test_unparseable_resets_at_yields_zero_and_keeps_the_payload():
    body = _real_response()
    body["five_hour"]["resets_at"] = "not-a-timestamp"
    client = _Client(get_response=_make_response(body=body))
    with patch("httpx.AsyncClient", return_value=client):
        payload = _run(poll_usage_endpoint("tok"))
    assert payload["sr"] == 0
    assert payload["s"] == 16


# ---------------------------------------------------------------------------
# Fallback: anything unexpected hands off to the header path
# ---------------------------------------------------------------------------

def _assert_header_payload(payload):
    """The values only the header fixture can produce."""
    assert payload is not None
    assert payload["s"] == 77 and payload["w"] == 88   # 0.77/0.88 fractions x100
    assert payload["sr"] == 10 and payload["wr"] == 20


@pytest.mark.parametrize("status", [400, 404, 418, 500, 502, 503])
def test_non_200_falls_back_to_header_path(status):
    client = _Client(get_response=_make_response(status_code=status),
                     post_response=_header_response())
    with patch("httpx.AsyncClient", return_value=client):
        payload = _run(poll_api("tok"))
    _assert_header_payload(payload)
    assert len(client.posts) == 1


def test_network_error_falls_back_to_header_path():
    import httpx

    client = _Client(get_error=httpx.ConnectError("no route to host"),
                     post_response=_header_response())
    with patch("httpx.AsyncClient", return_value=client):
        payload = _run(poll_api("tok"))
    _assert_header_payload(payload)


def test_malformed_json_falls_back_to_header_path():
    client = _Client(get_response=_make_response(raw_text="<html>nope</html>"),
                     post_response=_header_response())
    with patch("httpx.AsyncClient", return_value=client):
        payload = _run(poll_api("tok"))
    _assert_header_payload(payload)


@pytest.mark.parametrize("body", [
    pytest.param({}, id="empty-object"),
    pytest.param({"seven_day": {"utilization": 34.0}}, id="missing-five_hour"),
    pytest.param({"five_hour": {"utilization": 16.0}}, id="missing-seven_day"),
    pytest.param({"five_hour": None, "seven_day": None}, id="both-null-enterprise"),
    pytest.param({"five_hour": {"resets_at": "x"}, "seven_day": {"utilization": 1}},
                 id="missing-utilization"),
    pytest.param({"five_hour": {"utilization": "16"},
                  "seven_day": {"utilization": 34.0}}, id="utilization-a-string"),
    pytest.param({"five_hour": {"utilization": True},
                  "seven_day": {"utilization": 34.0}}, id="utilization-a-bool"),
    pytest.param([], id="a-list"),
    pytest.param("nope", id="a-string"),
    pytest.param(None, id="null"),
])
def test_unrecognised_shape_falls_back_to_header_path(body):
    client = _Client(get_response=_make_response(body=body),
                     post_response=_header_response())
    with patch("httpx.AsyncClient", return_value=client):
        payload = _run(poll_api("tok"))
    _assert_header_payload(payload)


def test_enterprise_account_still_reports_ent_via_the_header_path():
    """Requirement 4: only the header path can detect overage accounts.

    An Enterprise response has no five_hour/seven_day objects, so the endpoint
    declines and the overage headers produce the "ent" payload unchanged —
    including the billing-period fields the endpoint knows nothing about.
    """
    ent_headers = _make_response(
        status_code=200,
        headers={
            "anthropic-ratelimit-unified-overage-utilization": "0.55",
            "anthropic-ratelimit-unified-overage-reset": str(FROZEN_NOW + 86400),
            "anthropic-ratelimit-unified-status": "allowed",
        },
    )
    client = _Client(get_response=_make_response(body={"five_hour": None}),
                     post_response=ent_headers)
    with patch("httpx.AsyncClient", return_value=client):
        payload = _run(poll_api("tok"))
    assert payload["acct"] == "ent"
    assert payload["s"] == 55
    assert "tp" in payload and "pd" in payload   # _billing_period_info survived


def test_fallback_payload_is_returned_when_endpoint_returns_none_directly():
    """Sanity: poll_api's composition uses the module-global lookup."""
    client = _Client(post_response=_header_response())
    with patch("httpx.AsyncClient", return_value=client), \
         patch.object(mod, "poll_usage_endpoint", new=AsyncMock(return_value=None)):
        payload = _run(poll_api("tok"))
    _assert_header_payload(payload)
    assert client.gets == []


# ---------------------------------------------------------------------------
# Dead token: 401/403 must raise TokenExpired, exactly as poll_api does
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("status", [401, 403])
def test_dead_token_raises_token_expired(status):
    client = _Client(get_response=_make_response(status_code=status))
    with patch("httpx.AsyncClient", return_value=client):
        with pytest.raises(TokenExpired):
            _run(poll_usage_endpoint("tok"))


@pytest.mark.parametrize("status", [401, 403])
def test_dead_token_propagates_through_poll_api_without_spending_a_token(status):
    client = _Client(get_response=_make_response(status_code=status),
                     post_response=_header_response())
    with patch("httpx.AsyncClient", return_value=client):
        with pytest.raises(TokenExpired):
            _run(poll_api("tok"))
    assert client.posts == [], "spent a Messages request on a known-dead token"


def test_dead_token_does_not_bench_the_endpoint():
    """A 401 is about the token, not the endpoint — no cooldown."""
    client = _Client(get_response=_make_response(status_code=401))
    with patch("httpx.AsyncClient", return_value=client):
        with pytest.raises(TokenExpired):
            _run(poll_usage_endpoint("tok"))
    assert mod._usage_endpoint_benched_until == 0.0


# ---------------------------------------------------------------------------
# 429: bench the endpoint for USAGE_ENDPOINT_COOLDOWN_S
#
# This is a real past failure, not a hypothetical — two earlier attempts at
# this feature were reverted upstream (PRs #29/#37) for hammering the endpoint.
# ---------------------------------------------------------------------------

def test_cooldown_constant_is_fifteen_minutes():
    assert mod.USAGE_ENDPOINT_COOLDOWN_S == 900


def test_429_sets_the_cooldown_and_falls_back():
    client = _Client(get_response=_make_response(status_code=429),
                     post_response=_header_response())
    with patch("httpx.AsyncClient", return_value=client):
        payload = _run(poll_api("tok"))
    _assert_header_payload(payload)
    assert mod._usage_endpoint_benched_until == FROZEN_NOW + 900


def test_benched_endpoint_is_not_called_again_and_headers_carry_the_daemon(monkeypatch):
    client = _Client(get_response=_make_response(status_code=429),
                     post_response=_header_response())
    with patch("httpx.AsyncClient", return_value=client):
        _run(poll_api("tok"))                       # 1st poll: the 429
        assert len(client.gets) == 1

        for _ in range(3):                          # further polls while benched
            payload = _run(poll_api("tok"))
            _assert_header_payload(payload)
        assert len(client.gets) == 1, "hit the endpoint while it was benched"
        assert len(client.posts) == 4               # header path served all four


def test_endpoint_is_retried_once_the_cooldown_expires(monkeypatch):
    client = _Client(get_response=_make_response(status_code=429),
                     post_response=_header_response())
    with patch("httpx.AsyncClient", return_value=client):
        _run(poll_api("tok"))                       # benched until +900s
    assert len(client.gets) == 1

    # Still benched one second before the deadline...
    monkeypatch.setattr(mod.time, "time", lambda: FROZEN_NOW + 899)
    with patch("httpx.AsyncClient", return_value=client):
        _run(poll_api("tok"))
    assert len(client.gets) == 1

    # ...and back in play once it passes.
    client.get_response = _make_response(body=_real_response())
    monkeypatch.setattr(mod.time, "time", lambda: FROZEN_NOW + 901)
    with patch("httpx.AsyncClient", return_value=client):
        payload = _run(poll_api("tok"))
    assert len(client.gets) == 2
    assert payload["s"] == 16, "endpoint answered but its payload wasn't used"


def test_a_later_429_re_benches_from_the_new_now(monkeypatch):
    client = _Client(get_response=_make_response(status_code=429),
                     post_response=_header_response())
    with patch("httpx.AsyncClient", return_value=client):
        _run(poll_api("tok"))
    monkeypatch.setattr(mod.time, "time", lambda: FROZEN_NOW + 1000)
    with patch("httpx.AsyncClient", return_value=client):
        _run(poll_api("tok"))
    assert mod._usage_endpoint_benched_until == FROZEN_NOW + 1000 + 900


# ---------------------------------------------------------------------------
# Request shape — the GET must carry auth and no body
# ---------------------------------------------------------------------------

def test_request_sends_bearer_token_and_oauth_beta_header():
    client = _Client(get_response=_make_response(body=_real_response()))
    with patch("httpx.AsyncClient", return_value=client):
        _run(poll_usage_endpoint("TOK_ABC"))
    _url, kwargs = client.gets[0]
    headers = kwargs["headers"]
    assert headers["Authorization"] == "Bearer TOK_ABC"
    assert headers["anthropic-beta"] == "oauth-2025-04-20"
    assert "Content-Type" not in headers, "a GET must not claim a JSON body"
    assert "json" not in kwargs and "content" not in kwargs


def test_headers_template_is_not_mutated_between_calls():
    """Regression guard: a shared dict would leak one account's token."""
    client = _Client(get_response=_make_response(body=_real_response()))
    with patch("httpx.AsyncClient", return_value=client):
        _run(poll_usage_endpoint("TOK_A"))
        _run(poll_usage_endpoint("TOK_B"))
    assert "Authorization" not in mod.USAGE_HEADERS_TEMPLATE
    assert client.gets[1][1]["headers"]["Authorization"] == "Bearer TOK_B"
