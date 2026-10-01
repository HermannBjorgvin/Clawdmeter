#!/usr/bin/env python3
"""Tests for the opt-in Enterprise spend amount (mu/ml/mx/cur) in both Python daemons.

/api/oauth/usage was adopted once (PR #29) and reverted (PR #37) after a 429
retry loop, so beyond parsing these pin the rate-limit guards: one GET per
SPEND_CACHE_TTL, none during the 429 cooldown, none for Pro or with `spend` off,
and a usage-endpoint 401 never reaching the auth-error path. httpx is mocked.

Run: python -m pytest daemon/tests/test_spend.py -q
"""
import asyncio
import json
import time
from unittest.mock import AsyncMock, MagicMock, patch

import httpx
import pytest

import daemon.claude_usage_daemon as macos
import daemon.claude_usage_daemon_windows as win

SAMPLE = {
    "five_hour": None, "seven_day": None,
    "extra_usage": {"is_enabled": True, "monthly_limit": 50000, "used_credits": 1250.0,
                    "utilization": 2.5, "currency": "USD", "decimal_places": 2},
    "spend": {"used": {"amount_minor": 1250, "currency": "USD", "exponent": 2},
              "limit": {"amount_minor": 50000, "currency": "USD", "exponent": 2},
              "percent": 2, "enabled": True},
}
FIELDS = {"mu": 1250, "ml": 50000, "mx": 2, "cur": "USD"}

MODULES = pytest.mark.parametrize("mod", [macos, win], ids=["macos", "windows"])


def _run(coro):
    loop = asyncio.new_event_loop()
    try:
        return loop.run_until_complete(coro)
    finally:
        loop.close()


def _headers_resp(headers):
    resp = MagicMock()
    resp.status_code = 200
    resp.text = "mocked"
    resp.headers = MagicMock()
    resp.headers.get = lambda name, default=None: headers.get(name.lower(), default)
    return resp


def _ent_resp():
    return _headers_resp({
        "anthropic-ratelimit-unified-overage-utilization": "0.02",
        "anthropic-ratelimit-unified-overage-reset": str(time.time() + 20 * 86400),
        "anthropic-ratelimit-unified-status": "allowed",
    })


def _pro_resp():
    now = time.time()
    return _headers_resp({
        "anthropic-ratelimit-unified-5h-utilization": "0.42",
        "anthropic-ratelimit-unified-5h-reset": str(now + 3600),
        "anthropic-ratelimit-unified-7d-utilization": "0.10",
        "anthropic-ratelimit-unified-7d-reset": str(now + 86400),
        "anthropic-ratelimit-unified-5h-status": "allowed",
    })


def _usage_resp(status=200, body=SAMPLE):
    resp = MagicMock()
    resp.status_code = status
    resp.text = json.dumps(body)
    resp.json = MagicMock(return_value=body)
    return resp


def _client(post_resp, get=None):
    """Mock httpx.AsyncClient: post -> the /v1/messages poll, get -> /api/oauth/usage."""
    client = AsyncMock()
    client.__aenter__ = AsyncMock(return_value=client)
    client.__aexit__ = AsyncMock(return_value=False)
    client.post = AsyncMock(return_value=post_resp)
    client.get = get if get is not None else AsyncMock(return_value=_usage_resp())
    return client


def _poll(mod, client, token="tok-ent"):
    with patch("httpx.AsyncClient", return_value=client):
        return _run(mod.poll_api(token))


@pytest.fixture(autouse=True)
def _fresh_state(tmp_path, monkeypatch):
    """Isolate each test: empty spend cache, no cooldown, `spend = on` config."""
    cfg = tmp_path / "config"
    cfg.write_text("spend = on\n")
    for mod in (macos, win):
        monkeypatch.setattr(mod, "CONFIG_FILE", cfg)
        monkeypatch.setattr(mod, "_spend_cache", {})
        monkeypatch.setattr(mod, "_spend_cooldown_until", 0.0)
    return cfg


# --- parsing ---------------------------------------------------------------

@MODULES
def test_parse_prefers_spend_block(mod):
    body = json.loads(json.dumps(SAMPLE))
    body["extra_usage"]["used_credits"] = 1.0  # must be ignored while spend is valid
    assert mod.parse_spend(body) == FIELDS


@MODULES
def test_parse_falls_back_to_extra_usage(mod):
    body = {"extra_usage": {"monthly_limit": 50000, "used_credits": 1234.0,
                            "currency": "eur", "decimal_places": 2}}
    assert mod.parse_spend(body) == {"mu": 1234, "ml": 50000, "mx": 2, "cur": "EUR"}


@MODULES
def test_parse_falls_back_when_spend_block_is_bad(mod):
    body = {"spend": {"used": {"amount_minor": "x"}, "limit": {}},
            "extra_usage": SAMPLE["extra_usage"]}
    assert mod.parse_spend(body) == FIELDS


@MODULES
@pytest.mark.parametrize("body", [
    None, [], "x", {}, {"spend": None, "extra_usage": None},
    {"spend": {"used": {"amount_minor": 1, "currency": "USD", "exponent": 2},
               "limit": {"amount_minor": 0, "currency": "USD", "exponent": 2}}},
    {"extra_usage": {"monthly_limit": 0, "used_credits": 5, "currency": "USD", "decimal_places": 2}},
    {"extra_usage": {"monthly_limit": 100, "used_credits": 5, "decimal_places": 2}},
    {"extra_usage": {"monthly_limit": 100, "used_credits": -5, "currency": "USD", "decimal_places": 2}},
    {"extra_usage": {"monthly_limit": 100, "used_credits": True, "currency": "USD", "decimal_places": 2}},
    {"extra_usage": {"monthly_limit": 100, "used_credits": 5, "currency": "USDX", "decimal_places": 2}},
    {"extra_usage": {"monthly_limit": float("inf"), "used_credits": 5, "currency": "USD", "decimal_places": 2}},
], ids=lambda b: json.dumps(b, default=str)[:40])
def test_parse_malformed_or_zero_limit_yields_nothing(mod, body):
    assert mod.parse_spend(body) == {}


@MODULES
def test_format_spend(mod):
    assert mod.format_spend(FIELDS) == "$12.50/$500.00"
    assert mod.format_spend({"mu": 1500, "ml": 100000, "mx": 0, "cur": "JPY"}) == "JPY 1500/JPY 100000"


# --- payload ---------------------------------------------------------------

@MODULES
def test_enterprise_payload_gains_spend_fields(mod):
    client = _client(_ent_resp())
    payload = _poll(mod, client)
    assert payload["acct"] == "ent"
    assert {k: payload[k] for k in FIELDS} == FIELDS
    client.get.assert_awaited_once()
    args, kwargs = client.get.call_args
    assert args[0] == mod.USAGE_URL
    assert kwargs["headers"]["Authorization"] == "Bearer tok-ent"
    assert kwargs["headers"]["anthropic-beta"] == "oauth-2025-04-20"


@MODULES
def test_pro_payload_unchanged_and_usage_never_called(mod):
    client = _client(_pro_resp())
    payload = _poll(mod, client)
    assert set(payload) == {"s", "sr", "w", "wr", "st", "acct", "ok"}
    assert payload["acct"] == "pro"
    client.get.assert_not_called()


@MODULES
@pytest.mark.parametrize("cfg_text", ["", "spend = off\n", "spend = maybe\n"])
def test_spend_off_or_unset_never_calls_usage(mod, _fresh_state, cfg_text):
    _fresh_state.write_text(cfg_text)
    client = _client(_ent_resp())
    payload = _poll(mod, client)
    assert payload["acct"] == "ent"
    assert not set(FIELDS) & set(payload)
    client.get.assert_not_called()


# --- rate-limit safety -----------------------------------------------------

@MODULES
def test_cache_prevents_second_fetch_within_ttl(mod, monkeypatch):
    client = _client(_ent_resp())
    first = _poll(mod, client)
    second = _poll(mod, client)
    assert client.get.await_count == 1
    assert {k: second[k] for k in FIELDS} == {k: first[k] for k in FIELDS} == FIELDS
    # Once the cache is older than the TTL, exactly one more fetch happens.
    monkeypatch.setattr(mod, "SPEND_CACHE_TTL", 0)
    _poll(mod, client)
    assert client.get.await_count == 2


@MODULES
def test_429_sets_cooldown_and_blocks_requests(mod, monkeypatch):
    client = _client(_ent_resp())
    _poll(mod, client)                                   # seed the cache
    monkeypatch.setattr(mod, "SPEND_CACHE_TTL", 0)       # TTL no longer gates
    client.get = AsyncMock(return_value=_usage_resp(429, {"error": "rate_limited"}))
    payload = _poll(mod, client)
    assert client.get.await_count == 1
    assert mod._spend_cooldown_until > time.time() + mod.SPEND_COOLDOWN - 10
    assert {k: payload[k] for k in FIELDS} == FIELDS     # last cached fields reused
    # During cooldown: no request at all, for this token or any other.
    payload = _poll(mod, client)
    _poll(mod, client, token="tok-other")
    assert client.get.await_count == 1
    assert {k: payload[k] for k in FIELDS} == FIELDS


@MODULES
def test_429_without_cache_omits_fields(mod):
    client = _client(_ent_resp(), get=AsyncMock(return_value=_usage_resp(429, {})))
    payload = _poll(mod, client)
    assert payload["acct"] == "ent" and not set(FIELDS) & set(payload)


@MODULES
@pytest.mark.parametrize("status", [401, 403, 500])
def test_usage_http_error_is_not_auth_error(mod, status):
    client = _client(_ent_resp(), get=AsyncMock(return_value=_usage_resp(status, {"error": "x"})))
    payload = _poll(mod, client)  # must not raise TokenExpired / AuthError
    assert payload is not None and payload["ok"] is True and payload["acct"] == "ent"
    assert not set(FIELDS) & set(payload)
    client.get.assert_awaited_once()
    # The failure is remembered: no retry on the next poll within the TTL.
    _poll(mod, client)
    client.get.assert_awaited_once()


@MODULES
@pytest.mark.parametrize("make_get", [
    lambda: AsyncMock(side_effect=httpx.ConnectError("refused")),
    lambda: AsyncMock(side_effect=RuntimeError("boom")),
], ids=["network", "unexpected"])
def test_usage_exception_never_raises(mod, make_get):
    client = _client(_ent_resp(), get=make_get())
    payload = _poll(mod, client)
    assert payload["acct"] == "ent" and not set(FIELDS) & set(payload)


@MODULES
def test_usage_bad_json_omits_fields(mod):
    resp = _usage_resp()
    resp.json = MagicMock(side_effect=ValueError("not json"))
    client = _client(_ent_resp(), get=AsyncMock(return_value=resp))
    payload = _poll(mod, client)
    assert payload["acct"] == "ent" and not set(FIELDS) & set(payload)


@MODULES
def test_failure_after_success_keeps_last_good_fields(mod, monkeypatch):
    client = _client(_ent_resp())
    _poll(mod, client)
    monkeypatch.setattr(mod, "SPEND_CACHE_TTL", 0)
    client.get = AsyncMock(return_value=_usage_resp(200, {"spend": None}))
    payload = _poll(mod, client)
    assert {k: payload[k] for k in FIELDS} == FIELDS


@MODULES
def test_spend_fetch_does_not_log_token(mod, capsys):
    secret = "sk-ant-secret-spend-token"
    client = _client(_ent_resp(), get=AsyncMock(side_effect=httpx.ConnectError("refused")))
    _poll(mod, client, token=secret)
    out = capsys.readouterr()
    assert secret not in out.out and secret not in out.err


def test_sample_payload_wire_json_macos():
    """Pins the exact wire JSON for the documented sample (keys other than the
    time-dependent sr/tp/pd/rd)."""
    client = _client(_ent_resp())
    payload = _poll(macos, client)
    wire = json.dumps(payload, separators=(",", ":"))
    assert wire.endswith('"ok":true,"mu":1250,"ml":50000,"mx":2,"cur":"USD"}')
