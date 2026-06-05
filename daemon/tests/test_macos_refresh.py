#!/usr/bin/env python3
"""Unit tests for the macOS daemon's OAuth self-refresh logic.

Covers the pure, side-effect-free helpers that back token renewal:
  - _decode_keychain_blob   (hex-dump recovery)
  - _credentials_expiry_seconds  (expiresAt ms -> seconds)
  - _apply_refresh_response (OAuth 200 body -> stored credential dict)

Pure helpers are tested directly; the one test that drives
refresh_access_token mocks httpx and the credential read, so the live network
POST and real Keychain I/O are never hit.

Run: python -m pytest daemon/tests/test_macos_refresh.py -x -q
"""
import asyncio
import json
import time
from pathlib import Path
from unittest.mock import AsyncMock, MagicMock, patch

import pytest

# BLE deps are imported at module load; skip cleanly where they're unavailable.
pytest.importorskip("bleak")
pytest.importorskip("httpx")

from daemon.claude_usage_daemon import (  # noqa: E402
    _apply_refresh_response,
    _credentials_expiry_seconds,
    _decode_keychain_blob,
    _extract_access_token,
)

FIXTURES = Path(__file__).parent / "fixtures"


# --- _decode_keychain_blob --------------------------------------------------

def test_decode_passthrough_plain_json():
    """A normal JSON blob is never valid hex, so it passes through untouched."""
    blob = '{"claudeAiOauth": {"accessToken": "x"}}'
    assert _decode_keychain_blob(blob) == blob


def test_decode_hex_dump_round_trips():
    """An embedded newline makes `security -w` emit hex; we decode it back."""
    original = '{"claudeAiOauth": {"accessToken": "x"}}\n'
    hex_dump = original.encode("utf-8").hex()  # what `security -w` would print
    assert _decode_keychain_blob(hex_dump) == original


def test_decode_odd_length_is_not_treated_as_hex():
    """Odd-length all-hex-looking strings aren't valid hex; pass through."""
    assert _decode_keychain_blob("abc") == "abc"


# --- _credentials_expiry_seconds --------------------------------------------

def test_expiry_milliseconds_normalized_to_seconds():
    raw = (FIXTURES / "credentials_nested.json").read_text()
    secs = _credentials_expiry_seconds(raw)
    assert secs == 9999999999.0  # 9999999999000 ms -> seconds


def test_expiry_missing_returns_none():
    assert _credentials_expiry_seconds('{"claudeAiOauth": {}}') is None


def test_expiry_bad_blob_returns_none():
    # A non-None argument is treated as the blob itself (None is the sentinel
    # for "read the live store", so it's deliberately not tested here).
    assert _credentials_expiry_seconds("not json") is None
    assert _credentials_expiry_seconds("") is None


# --- _apply_refresh_response ------------------------------------------------

def test_apply_rotates_all_fields():
    oauth = {"accessToken": "OLD", "refreshToken": "OLD_R", "expiresAt": 1,
             "scopes": ["user:inference"], "keep": "untouched"}
    t0 = time.time()
    ret = _apply_refresh_response(
        oauth, {"access_token": "NEW", "refresh_token": "NEW_R", "expires_in": 28800})
    assert ret == "NEW"
    assert oauth["accessToken"] == "NEW"
    assert oauth["refreshToken"] == "NEW_R"
    assert abs(oauth["expiresAt"] / 1000 - (t0 + 28800)) < 5
    # Unrelated fields are preserved verbatim.
    assert oauth["scopes"] == ["user:inference"]
    assert oauth["keep"] == "untouched"


def test_apply_preserves_refresh_token_when_not_rotated():
    oauth = {"accessToken": "A", "refreshToken": "KEEP", "expiresAt": 1}
    _apply_refresh_response(oauth, {"access_token": "A2", "expires_in": 100})
    assert oauth["accessToken"] == "A2"
    assert oauth["refreshToken"] == "KEEP"


def test_apply_missing_access_token_is_noop():
    oauth = {"accessToken": "A", "refreshToken": "R"}
    assert _apply_refresh_response(oauth, {"error": "nope"}) is None
    assert oauth == {"accessToken": "A", "refreshToken": "R"}


def test_apply_result_serializes_cleanly():
    """The mutated dict must serialize to single-line JSON (no newline that
    would re-trigger the Keychain hex-dump path on the next read)."""
    data = {"mcpOAuth": {"x": 1},
            "claudeAiOauth": {"accessToken": "OLD", "refreshToken": "R", "expiresAt": 1}}
    _apply_refresh_response(data["claudeAiOauth"], {"access_token": "NEW", "expires_in": 60})
    blob = json.dumps(data)
    assert "\n" not in blob
    assert json.loads(blob)["claudeAiOauth"]["accessToken"] == "NEW"
    assert json.loads(blob)["mcpOAuth"] == {"x": 1}  # sibling untouched


# --- _extract_access_token (sanity, shared with read path) ------------------

def test_extract_nested_shape():
    blob = (FIXTURES / "credentials_nested.json").read_text()
    assert _extract_access_token(blob) == "sk-ant-test-1234"


# --- refresh cooldown (anti-hammering guard) --------------------------------

def test_refresh_cooldown_blocks_rapid_reattempts(monkeypatch):
    """Two refresh calls within REFRESH_COOLDOWN: the second is skipped with no
    network call. This is the guard that stops a data-hungry device from driving
    the OAuth endpoint into a 429 hammer loop."""
    import daemon.claude_usage_daemon as mod

    creds = json.dumps({"claudeAiOauth":
                        {"accessToken": "OLD", "refreshToken": "R", "expiresAt": 1}})
    monkeypatch.setattr(mod, "_read_credentials_raw", lambda: creds)
    monkeypatch.setattr(mod, "_write_credentials_raw", lambda blob: True)
    monkeypatch.setattr(mod, "_last_refresh_attempt", 0.0)  # let the first attempt through

    resp = MagicMock(status_code=200)
    resp.json = MagicMock(return_value={"access_token": "NEW", "expires_in": 28800})
    client = AsyncMock()
    client.__aenter__ = AsyncMock(return_value=client)
    client.__aexit__ = AsyncMock(return_value=False)
    client.post = AsyncMock(return_value=resp)

    with patch("httpx.AsyncClient", return_value=client):
        first = asyncio.run(mod.refresh_access_token())    # allowed -> 200 -> "NEW"
        second = asyncio.run(mod.refresh_access_token())   # within cooldown -> skipped
    assert first == "NEW"
    assert second is None
    client.post.assert_awaited_once()  # exactly one network call despite two calls


def _mock_oauth_client(status_code, json_body):
    """An httpx.AsyncClient mock whose POST returns a canned OAuth response."""
    resp = MagicMock(status_code=status_code)
    resp.text = "mocked"
    resp.json = MagicMock(return_value=json_body)
    client = AsyncMock()
    client.__aenter__ = AsyncMock(return_value=client)
    client.__aexit__ = AsyncMock(return_value=False)
    client.post = AsyncMock(return_value=resp)
    return client


def test_refresh_backoff_grows_and_skips_within_window(monkeypatch):
    """A failed (429) refresh increments the failure counter; the next call within
    the now-doubled window is skipped without a second network call."""
    import daemon.claude_usage_daemon as mod
    creds = json.dumps({"claudeAiOauth": {"accessToken": "OLD", "refreshToken": "R", "expiresAt": 1}})
    monkeypatch.setattr(mod, "_read_credentials_raw", lambda: creds)
    monkeypatch.setattr(mod, "_write_credentials_raw", lambda b: True)
    monkeypatch.setattr(mod, "_last_refresh_attempt", 0.0)
    monkeypatch.setattr(mod, "_refresh_failures", 0)

    client = _mock_oauth_client(429, {})
    with patch("httpx.AsyncClient", return_value=client):
        first = asyncio.run(mod.refresh_access_token())    # fires -> 429 -> failure
    assert first is None
    assert mod._refresh_failures == 1                       # counter grew

    client.post.reset_mock()
    with patch("httpx.AsyncClient", return_value=client):
        second = asyncio.run(mod.refresh_access_token())   # within backoff -> skipped
    assert second is None
    client.post.assert_not_awaited()                        # no second hit on the endpoint


def test_refresh_success_resets_backoff(monkeypatch):
    """A 200 clears the accumulated failure count so the window returns to base."""
    import daemon.claude_usage_daemon as mod
    creds = json.dumps({"claudeAiOauth": {"accessToken": "OLD", "refreshToken": "R", "expiresAt": 1}})
    monkeypatch.setattr(mod, "_read_credentials_raw", lambda: creds)
    monkeypatch.setattr(mod, "_write_credentials_raw", lambda b: True)
    monkeypatch.setattr(mod, "_last_refresh_attempt", 0.0)
    monkeypatch.setattr(mod, "_refresh_failures", 3)        # pretend we've been failing a while

    client = _mock_oauth_client(200, {"access_token": "NEW", "expires_in": 28800})
    with patch("httpx.AsyncClient", return_value=client):
        new = asyncio.run(mod.refresh_access_token())
    assert new == "NEW"
    assert mod._refresh_failures == 0                       # success cleared the backoff
