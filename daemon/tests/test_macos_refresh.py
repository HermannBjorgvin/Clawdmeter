#!/usr/bin/env python3
"""Unit tests for the macOS daemon's OAuth self-refresh logic.

Covers the pure, side-effect-free helpers that back token renewal:
  - _decode_keychain_blob   (hex-dump recovery)
  - _credentials_expiry_seconds  (expiresAt ms -> seconds)
  - _apply_refresh_response (OAuth 200 body -> stored credential dict)

Network (the actual POST) and Keychain I/O are intentionally NOT exercised
here — those are validated manually against the live endpoint.

Run: python -m pytest daemon/tests/test_macos_refresh.py -x -q
"""
import json
import time
from pathlib import Path

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
