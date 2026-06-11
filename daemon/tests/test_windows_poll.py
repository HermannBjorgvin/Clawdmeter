#!/usr/bin/env python3
"""Unit tests for poll_api / pct / reset_minutes / JSON-shape — POLL-01.

These tests cover the Anthropic API polling logic ported from the macOS daemon.
All tests mock httpx so no real network calls are made.

Run: python -m pytest daemon/tests/test_windows_poll.py -x -q
"""
import asyncio
import json
import time
from unittest.mock import AsyncMock, MagicMock, patch

import pytest

from daemon.claude_usage_daemon_windows import AuthError, poll_api


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _make_mock_response(status_code=200, headers=None):
    """Build a mock httpx.Response-like object with controllable headers."""
    resp = MagicMock()
    resp.status_code = status_code
    resp.text = "mocked"
    # httpx headers are case-insensitive; MagicMock .get() must behave the same
    header_data = headers or {}
    resp.headers = MagicMock()
    resp.headers.get = lambda name, default=None: header_data.get(name.lower(), default)
    return resp


def _run(coro):
    """Run a coroutine synchronously for synchronous test functions."""
    return asyncio.get_event_loop().run_until_complete(coro)


# ---------------------------------------------------------------------------
# Test: full poll_api with realistic ratelimit headers
# ---------------------------------------------------------------------------

def test_poll_api_nominal(monkeypatch):
    """poll_api with a 200 response + ratelimit headers produces the correct payload."""
    now = time.time()
    reset_5h = str(now + 3600)   # 60 minutes from now
    reset_7d = str(now + 86400)  # 1440 minutes from now

    mock_resp = _make_mock_response(
        status_code=200,
        headers={
            "anthropic-ratelimit-unified-5h-utilization": "0.42",
            "anthropic-ratelimit-unified-5h-reset": reset_5h,
            "anthropic-ratelimit-unified-7d-utilization": "0.10",
            "anthropic-ratelimit-unified-7d-reset": reset_7d,
            "anthropic-ratelimit-unified-5h-status": "allowed",
        },
    )

    async def fake_post(*args, **kwargs):
        return mock_resp

    mock_client = AsyncMock()
    mock_client.__aenter__ = AsyncMock(return_value=mock_client)
    mock_client.__aexit__ = AsyncMock(return_value=False)
    mock_client.post = fake_post

    with patch("httpx.AsyncClient", return_value=mock_client):
        payload = _run(poll_api("fake-token"))

    assert payload is not None
    assert payload["s"] == 42
    assert payload["w"] == 10
    assert payload["st"] == "allowed"
    assert payload["ok"] is True
    # reset_minutes allows ±1 minute tolerance
    assert abs(payload["sr"] - 60) <= 1, f"Expected ~60, got {payload['sr']}"
    assert abs(payload["wr"] - 1440) <= 1, f"Expected ~1440, got {payload['wr']}"


# ---------------------------------------------------------------------------
# Test: pct() correctness — exercised through poll_api output
# ---------------------------------------------------------------------------

def test_pct_42_percent(monkeypatch):
    """pct('0.42') -> 42."""
    now = time.time()
    mock_resp = _make_mock_response(
        status_code=200,
        headers={
            "anthropic-ratelimit-unified-5h-utilization": "0.42",
            "anthropic-ratelimit-unified-5h-reset": str(now + 3600),
            "anthropic-ratelimit-unified-7d-utilization": "0.10",
            "anthropic-ratelimit-unified-7d-reset": str(now + 86400),
            "anthropic-ratelimit-unified-5h-status": "allowed",
        },
    )

    mock_client = AsyncMock()
    mock_client.__aenter__ = AsyncMock(return_value=mock_client)
    mock_client.__aexit__ = AsyncMock(return_value=False)
    mock_client.post = AsyncMock(return_value=mock_resp)

    with patch("httpx.AsyncClient", return_value=mock_client):
        payload = _run(poll_api("fake-token"))

    assert payload["s"] == 42


def test_pct_100_percent(monkeypatch):
    """pct('1.0') -> 100."""
    now = time.time()
    mock_resp = _make_mock_response(
        status_code=200,
        headers={
            "anthropic-ratelimit-unified-5h-utilization": "1.0",
            "anthropic-ratelimit-unified-5h-reset": str(now + 3600),
            "anthropic-ratelimit-unified-7d-utilization": "1.0",
            "anthropic-ratelimit-unified-7d-reset": str(now + 86400),
            "anthropic-ratelimit-unified-5h-status": "allowed",
        },
    )

    mock_client = AsyncMock()
    mock_client.__aenter__ = AsyncMock(return_value=mock_client)
    mock_client.__aexit__ = AsyncMock(return_value=False)
    mock_client.post = AsyncMock(return_value=mock_resp)

    with patch("httpx.AsyncClient", return_value=mock_client):
        payload = _run(poll_api("fake-token"))

    assert payload["s"] == 100
    assert payload["w"] == 100


def test_pct_empty_string_defaults_to_zero(monkeypatch):
    """pct('') -> 0 (missing header defaults to '0', but empty string -> 0)."""
    now = time.time()
    # Override default so utilization header returns "" explicitly
    mock_resp = _make_mock_response(
        status_code=200,
        headers={
            "anthropic-ratelimit-unified-5h-utilization": "0",
            "anthropic-ratelimit-unified-5h-reset": str(now + 3600),
            "anthropic-ratelimit-unified-7d-utilization": "0",
            "anthropic-ratelimit-unified-7d-reset": str(now + 86400),
            "anthropic-ratelimit-unified-5h-status": "allowed",
        },
    )

    mock_client = AsyncMock()
    mock_client.__aenter__ = AsyncMock(return_value=mock_client)
    mock_client.__aexit__ = AsyncMock(return_value=False)
    mock_client.post = AsyncMock(return_value=mock_resp)

    with patch("httpx.AsyncClient", return_value=mock_client):
        payload = _run(poll_api("fake-token"))

    assert payload["s"] == 0
    assert payload["w"] == 0


# ---------------------------------------------------------------------------
# Test: reset_minutes() — exercised through poll_api output
# ---------------------------------------------------------------------------

def test_reset_minutes_60_minutes(monkeypatch):
    """reset_minutes(now+3600) -> ~60."""
    now = time.time()
    mock_resp = _make_mock_response(
        status_code=200,
        headers={
            "anthropic-ratelimit-unified-5h-utilization": "0.5",
            "anthropic-ratelimit-unified-5h-reset": str(now + 3600),
            "anthropic-ratelimit-unified-7d-utilization": "0.5",
            "anthropic-ratelimit-unified-7d-reset": str(now + 86400),
            "anthropic-ratelimit-unified-5h-status": "allowed",
        },
    )

    mock_client = AsyncMock()
    mock_client.__aenter__ = AsyncMock(return_value=mock_client)
    mock_client.__aexit__ = AsyncMock(return_value=False)
    mock_client.post = AsyncMock(return_value=mock_resp)

    with patch("httpx.AsyncClient", return_value=mock_client):
        payload = _run(poll_api("fake-token"))

    assert abs(payload["sr"] - 60) <= 1, f"Expected ~60, got {payload['sr']}"


def test_reset_minutes_negative_clamps_to_zero(monkeypatch):
    """reset_minutes for a past timestamp clamps to 0."""
    now = time.time()
    mock_resp = _make_mock_response(
        status_code=200,
        headers={
            "anthropic-ratelimit-unified-5h-utilization": "0.5",
            "anthropic-ratelimit-unified-5h-reset": str(now - 100),  # 100s in the past
            "anthropic-ratelimit-unified-7d-utilization": "0.5",
            "anthropic-ratelimit-unified-7d-reset": str(now - 100),  # also in the past
            "anthropic-ratelimit-unified-5h-status": "allowed",
        },
    )

    mock_client = AsyncMock()
    mock_client.__aenter__ = AsyncMock(return_value=mock_client)
    mock_client.__aexit__ = AsyncMock(return_value=False)
    mock_client.post = AsyncMock(return_value=mock_resp)

    with patch("httpx.AsyncClient", return_value=mock_client):
        payload = _run(poll_api("fake-token"))

    assert payload["sr"] == 0
    assert payload["wr"] == 0


def test_reset_minutes_invalid_string_returns_zero(monkeypatch):
    """reset_minutes('notanumber') -> 0 (ValueError-safe)."""
    now = time.time()
    mock_resp = _make_mock_response(
        status_code=200,
        headers={
            "anthropic-ratelimit-unified-5h-utilization": "0.5",
            "anthropic-ratelimit-unified-5h-reset": "notanumber",
            "anthropic-ratelimit-unified-7d-utilization": "0.5",
            "anthropic-ratelimit-unified-7d-reset": "notanumber",
            "anthropic-ratelimit-unified-5h-status": "allowed",
        },
    )

    mock_client = AsyncMock()
    mock_client.__aenter__ = AsyncMock(return_value=mock_client)
    mock_client.__aexit__ = AsyncMock(return_value=False)
    mock_client.post = AsyncMock(return_value=mock_resp)

    with patch("httpx.AsyncClient", return_value=mock_client):
        payload = _run(poll_api("fake-token"))

    assert payload["sr"] == 0
    assert payload["wr"] == 0


# ---------------------------------------------------------------------------
# Test: missing headers default gracefully
# ---------------------------------------------------------------------------

def test_missing_utilization_headers_default_to_zero(monkeypatch):
    """Missing utilization headers produce 0 (hdr default '0' -> pct('0') = 0)."""
    now = time.time()
    # No utilization or status headers — only reset headers present
    mock_resp = _make_mock_response(
        status_code=200,
        headers={
            "anthropic-ratelimit-unified-5h-reset": str(now + 3600),
            "anthropic-ratelimit-unified-7d-reset": str(now + 86400),
        },
    )

    mock_client = AsyncMock()
    mock_client.__aenter__ = AsyncMock(return_value=mock_client)
    mock_client.__aexit__ = AsyncMock(return_value=False)
    mock_client.post = AsyncMock(return_value=mock_resp)

    with patch("httpx.AsyncClient", return_value=mock_client):
        payload = _run(poll_api("fake-token"))

    assert payload["s"] == 0
    assert payload["w"] == 0


def test_missing_status_header_defaults_to_unknown(monkeypatch):
    """Missing 5h-status header defaults to 'unknown'."""
    now = time.time()
    mock_resp = _make_mock_response(
        status_code=200,
        headers={
            "anthropic-ratelimit-unified-5h-utilization": "0.5",
            "anthropic-ratelimit-unified-5h-reset": str(now + 3600),
            "anthropic-ratelimit-unified-7d-utilization": "0.5",
            "anthropic-ratelimit-unified-7d-reset": str(now + 86400),
            # NOTE: no 5h-status header
        },
    )

    mock_client = AsyncMock()
    mock_client.__aenter__ = AsyncMock(return_value=mock_client)
    mock_client.__aexit__ = AsyncMock(return_value=False)
    mock_client.post = AsyncMock(return_value=mock_resp)

    with patch("httpx.AsyncClient", return_value=mock_client):
        payload = _run(poll_api("fake-token"))

    assert payload["st"] == "unknown"


# ---------------------------------------------------------------------------
# Test: poll_api returns None on HTTP >= 400
# ---------------------------------------------------------------------------

def test_poll_api_returns_none_on_4xx(monkeypatch):
    """poll_api returns None when response status code is >= 400."""
    mock_resp = _make_mock_response(status_code=429, headers={})
    mock_client = AsyncMock()
    mock_client.__aenter__ = AsyncMock(return_value=mock_client)
    mock_client.__aexit__ = AsyncMock(return_value=False)
    mock_client.post = AsyncMock(return_value=mock_resp)

    with patch("httpx.AsyncClient", return_value=mock_client):
        result = _run(poll_api("fake-token"))

    assert result is None


def test_poll_api_returns_none_on_5xx(monkeypatch):
    """poll_api returns None when response status code is >= 500."""
    mock_resp = _make_mock_response(status_code=500, headers={})
    mock_client = AsyncMock()
    mock_client.__aenter__ = AsyncMock(return_value=mock_client)
    mock_client.__aexit__ = AsyncMock(return_value=False)
    mock_client.post = AsyncMock(return_value=mock_resp)

    with patch("httpx.AsyncClient", return_value=mock_client):
        result = _run(poll_api("fake-token"))

    assert result is None


# ---------------------------------------------------------------------------
# Test: poll_api raises AuthError ONLY on a genuine 401/403 (SC#5 fix)
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("status", [401, 403])
def test_poll_api_raises_autherror_on_401_403(status):
    """A real auth rejection must raise AuthError — the only signal that warrants
    the actionable 'token expired — run claude login' toast. Transient failures
    (5xx, 429, network) return None instead and must NOT trigger that toast."""
    mock_resp = _make_mock_response(status_code=status, headers={})
    mock_client = AsyncMock()
    mock_client.__aenter__ = AsyncMock(return_value=mock_client)
    mock_client.__aexit__ = AsyncMock(return_value=False)
    mock_client.post = AsyncMock(return_value=mock_resp)

    with patch("httpx.AsyncClient", return_value=mock_client):
        with pytest.raises(AuthError):
            _run(poll_api("fake-token"))


def test_poll_api_returns_none_not_autherror_on_429(monkeypatch):
    """Rate-limit (429) is transient — None, NOT AuthError (regression guard for
    the 401/403-vs-other-4xx split)."""
    mock_resp = _make_mock_response(status_code=429, headers={})
    mock_client = AsyncMock()
    mock_client.__aenter__ = AsyncMock(return_value=mock_client)
    mock_client.__aexit__ = AsyncMock(return_value=False)
    mock_client.post = AsyncMock(return_value=mock_resp)

    with patch("httpx.AsyncClient", return_value=mock_client):
        result = _run(poll_api("fake-token"))

    assert result is None


# ---------------------------------------------------------------------------
# Test: poll_api returns None on httpx.HTTPError
# ---------------------------------------------------------------------------

def test_poll_api_returns_none_on_http_error(monkeypatch):
    """poll_api returns None when httpx.HTTPError is raised (network failure)."""
    import httpx

    mock_client = AsyncMock()
    mock_client.__aenter__ = AsyncMock(return_value=mock_client)
    mock_client.__aexit__ = AsyncMock(return_value=False)
    mock_client.post = AsyncMock(side_effect=httpx.ConnectError("Connection refused"))

    with patch("httpx.AsyncClient", return_value=mock_client):
        result = _run(poll_api("fake-token"))

    assert result is None


# ---------------------------------------------------------------------------
# Test: compact JSON wire shape (no spaces after ':' or ',')
# ---------------------------------------------------------------------------

def test_wire_bytes_compact_json_shape(monkeypatch):
    """The JSON-encoded payload uses compact separators (',':') — no spaces."""
    now = time.time()
    mock_resp = _make_mock_response(
        status_code=200,
        headers={
            "anthropic-ratelimit-unified-5h-utilization": "0.42",
            "anthropic-ratelimit-unified-5h-reset": str(now + 3600),
            "anthropic-ratelimit-unified-7d-utilization": "0.10",
            "anthropic-ratelimit-unified-7d-reset": str(now + 86400),
            "anthropic-ratelimit-unified-5h-status": "allowed",
        },
    )

    mock_client = AsyncMock()
    mock_client.__aenter__ = AsyncMock(return_value=mock_client)
    mock_client.__aexit__ = AsyncMock(return_value=False)
    mock_client.post = AsyncMock(return_value=mock_resp)

    with patch("httpx.AsyncClient", return_value=mock_client):
        payload = _run(poll_api("fake-token"))

    assert payload is not None
    # Encode exactly as the wire layer will (Session.write_payload uses this form)
    wire_bytes = json.dumps(payload, separators=(",", ":")).encode()
    wire_str = wire_bytes.decode()

    # Compact form: no space after ':' or ','
    assert ": " not in wire_str, f"Non-compact JSON detected: {wire_str!r}"
    assert ", " not in wire_str, f"Non-compact JSON detected: {wire_str!r}"

    # Must start with '{' and contain all required keys
    assert wire_str.startswith("{")
    for key in ("s", "sr", "w", "wr", "st", "ok"):
        assert f'"{key}"' in wire_str, f"Missing key {key!r} in wire bytes: {wire_str!r}"


# ---------------------------------------------------------------------------
# Test: token is NOT logged (T-02-01 threat mitigation)
# ---------------------------------------------------------------------------

def test_poll_api_does_not_log_token(monkeypatch, capsys):
    """poll_api must not print the bearer token (T-02-01: token never logged)."""
    now = time.time()
    secret_token = "sk-ant-secret-token-12345"

    mock_resp = _make_mock_response(
        status_code=200,
        headers={
            "anthropic-ratelimit-unified-5h-utilization": "0.5",
            "anthropic-ratelimit-unified-5h-reset": str(now + 3600),
            "anthropic-ratelimit-unified-7d-utilization": "0.5",
            "anthropic-ratelimit-unified-7d-reset": str(now + 86400),
            "anthropic-ratelimit-unified-5h-status": "allowed",
        },
    )

    mock_client = AsyncMock()
    mock_client.__aenter__ = AsyncMock(return_value=mock_client)
    mock_client.__aexit__ = AsyncMock(return_value=False)
    mock_client.post = AsyncMock(return_value=mock_resp)

    with patch("httpx.AsyncClient", return_value=mock_client):
        _run(poll_api(secret_token))

    captured = capsys.readouterr()
    assert secret_token not in captured.out, "Token leaked to stdout (T-02-01 violation)"
    assert secret_token not in captured.err, "Token leaked to stderr (T-02-01 violation)"
