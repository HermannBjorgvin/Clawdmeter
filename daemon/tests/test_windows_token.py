#!/usr/bin/env python3
"""Unit tests for daemon/claude_usage_daemon_windows.py — TOKEN-01.

Run: python -m pytest daemon/tests/test_windows_token.py -x -q
"""
import asyncio
import json
import subprocess
import sys
import time
from pathlib import Path
from unittest.mock import AsyncMock, MagicMock, patch

import pytest

from daemon.claude_usage_daemon_windows import (
    AuthError,
    _extract_access_token,
    _read_expiry,
    _windows_credential_candidates,
    read_token,
)


FIXTURES = Path(__file__).parent / "fixtures"


def _run(coro):
    """Run a coroutine synchronously for synchronous test functions."""
    return asyncio.run(coro)


def test_extract_nested_shape():
    """_extract_access_token handles the real Windows claudeAiOauth nested shape."""
    blob = (FIXTURES / "credentials_nested.json").read_text()
    assert _extract_access_token(blob) == "sk-ant-test-1234"


def test_extract_direct_shape():
    """_extract_access_token handles the legacy direct accessToken shape."""
    blob = (FIXTURES / "credentials_direct.json").read_text()
    assert _extract_access_token(blob) == "sk-ant-test-5678"


def test_read_token_env_override(tmp_path, monkeypatch):
    """read_token() honours CLAUDE_CREDENTIALS_PATH env override (D-03)."""
    creds = tmp_path / ".credentials.json"
    creds.write_text(json.dumps({"accessToken": "sk-ant-test-ENV"}))
    monkeypatch.setenv("CLAUDE_CREDENTIALS_PATH", str(creds))
    monkeypatch.delenv("CLAUDE_CONFIG_DIR", raising=False)
    assert read_token() == "sk-ant-test-ENV"


def test_read_token_primary_path(tmp_path, monkeypatch):
    """read_token() reads from the primary candidate path (first hit wins)."""
    creds = tmp_path / ".claude" / ".credentials.json"
    creds.parent.mkdir(parents=True)
    creds.write_text(json.dumps({"claudeAiOauth": {"accessToken": "sk-ant-test-PRIMARY"}}))
    monkeypatch.delenv("CLAUDE_CREDENTIALS_PATH", raising=False)
    monkeypatch.delenv("CLAUDE_CONFIG_DIR", raising=False)
    # Monkeypatch _windows_credential_candidates to return only our tmp path
    import daemon.claude_usage_daemon_windows as mod
    monkeypatch.setattr(mod, "_windows_credential_candidates", lambda: [creds])
    assert read_token() == "sk-ant-test-PRIMARY"


def test_read_token_localappdata_fallback(tmp_path, monkeypatch):
    """read_token() falls back to %LOCALAPPDATA%/Claude/.credentials.json when primary is absent."""
    missing_primary = tmp_path / "nonexistent_primary" / ".credentials.json"
    present_localappdata = tmp_path / "localappdata" / ".credentials.json"
    missing_appdata = tmp_path / "nonexistent_appdata" / ".credentials.json"

    present_localappdata.parent.mkdir(parents=True)
    present_localappdata.write_text(json.dumps({"accessToken": "sk-ant-test-LA"}))

    import daemon.claude_usage_daemon_windows as mod
    monkeypatch.setattr(
        mod,
        "_windows_credential_candidates",
        lambda: [missing_primary, present_localappdata, missing_appdata],
    )
    assert read_token() == "sk-ant-test-LA"


def test_read_token_appdata_fallback(tmp_path, monkeypatch):
    """read_token() falls back to %APPDATA%/Claude/.credentials.json when primary and LOCALAPPDATA are absent."""
    missing_primary = tmp_path / "nonexistent_primary" / ".credentials.json"
    missing_localappdata = tmp_path / "nonexistent_localappdata" / ".credentials.json"
    present_appdata = tmp_path / "appdata" / ".credentials.json"

    present_appdata.parent.mkdir(parents=True)
    present_appdata.write_text(json.dumps({"accessToken": "sk-ant-test-APP"}))

    import daemon.claude_usage_daemon_windows as mod
    monkeypatch.setattr(
        mod,
        "_windows_credential_candidates",
        lambda: [missing_primary, missing_localappdata, present_appdata],
    )
    assert read_token() == "sk-ant-test-APP"


def test_read_token_no_file(tmp_path, monkeypatch):
    """read_token() returns None when no credential file can be found."""
    monkeypatch.setenv("CLAUDE_CREDENTIALS_PATH", str(tmp_path / "nonexistent.json"))
    monkeypatch.delenv("CLAUDE_CONFIG_DIR", raising=False)
    assert read_token() is None


def test_read_token_config_dir_override(tmp_path, monkeypatch):
    """read_token() honours the official CLAUDE_CONFIG_DIR env override."""
    creds = tmp_path / ".credentials.json"
    creds.write_text(json.dumps({"accessToken": "sk-ant-test-CFGDIR"}))
    monkeypatch.setenv("CLAUDE_CONFIG_DIR", str(tmp_path))
    monkeypatch.delenv("CLAUDE_CREDENTIALS_PATH", raising=False)
    assert read_token() == "sk-ant-test-CFGDIR"


def test_read_expiry_decodes_milliseconds(monkeypatch):
    """_read_expiry() divides expiresAt by 1000 (ms -> s); fixture 9999999999000 -> year 2286."""
    monkeypatch.setenv("CLAUDE_CREDENTIALS_PATH", str(FIXTURES / "credentials_nested.json"))
    monkeypatch.delenv("CLAUDE_CONFIG_DIR", raising=False)
    result = _read_expiry()
    assert result.startswith("2286-"), f"Expected year 2286, got: {result}"


# --- WR-03: regression guard for CR-01 (empty/blank token must not be accepted) ---

def test_extract_empty_token_is_none():
    """_extract_access_token returns None for empty accessToken (CR-01 regression guard)."""
    assert _extract_access_token('{"accessToken": ""}') is None
    assert _extract_access_token('{}') is None


def test_read_token_empty_credential_file_returns_none(tmp_path, monkeypatch):
    """read_token() returns None (not empty string) when credential file has empty accessToken."""
    creds = tmp_path / ".credentials.json"
    creds.write_text(json.dumps({"accessToken": ""}))
    monkeypatch.setenv("CLAUDE_CREDENTIALS_PATH", str(creds))
    monkeypatch.delenv("CLAUDE_CONFIG_DIR", raising=False)
    assert read_token() is None


# --- WR-01: regression guard for _read_expiry with non-dict top-level JSON ---

def test_read_expiry_non_dict_json_returns_unknown(tmp_path, monkeypatch):
    """_read_expiry() returns 'expiry unknown' (not crash) for non-dict top-level JSON (WR-01)."""
    creds = tmp_path / ".credentials.json"
    creds.write_text("[1, 2, 3]")
    monkeypatch.setenv("CLAUDE_CREDENTIALS_PATH", str(creds))
    monkeypatch.delenv("CLAUDE_CONFIG_DIR", raising=False)
    assert _read_expiry() == "expiry unknown"


# --- WR-02: D-06 redaction requirement must be tested ---

def test_main_emits_linux_warning(monkeypatch):
    """__main__ prints a non-fatal stderr warning on non-Windows platforms (new async runner).

    Phase 2 replaced the Phase 1 token-printing __main__ with asyncio.run(main()). The
    new contract:
      - On non-Windows: emits "WinRT BLE will not be available" to stderr before the loop.
      - Enters the async scan/connect/poll loop (no longer prints token/expiry).
    This test interrupts the process after 3s to capture the warning without hanging.
    """
    env = {**__import__("os").environ, "CLAUDE_CREDENTIALS_PATH": str(FIXTURES / "credentials_nested.json")}
    env.pop("CLAUDE_CONFIG_DIR", None)
    module = str(Path(__file__).parent.parent / "claude_usage_daemon_windows.py")
    try:
        result = subprocess.run(
            [sys.executable, module],
            capture_output=True,
            text=True,
            env=env,
            timeout=3,
        )
        # If it exits cleanly, verify warning was emitted
        assert "WinRT BLE will not be available" in result.stderr
    except subprocess.TimeoutExpired as exc:
        # Process is hanging in the scan loop — expected behavior on Linux.
        # The warning should appear in the partial stderr captured so far.
        partial_stderr = (exc.stderr or b"").decode("utf-8", errors="replace") if isinstance(exc.stderr, bytes) else (exc.stderr or "")
        assert "WinRT BLE will not be available" in partial_stderr, (
            f"Expected Linux/WSL warning in stderr before scan loop, got: {partial_stderr!r}"
        )


def test_main_emits_linux_warning_before_loop(monkeypatch):
    """__main__ stderr warning appears before the async scan loop starts on Linux/WSL."""
    env = {**__import__("os").environ}
    env.pop("CLAUDE_CONFIG_DIR", None)
    env.pop("CLAUDE_CREDENTIALS_PATH", None)
    module = str(Path(__file__).parent.parent / "claude_usage_daemon_windows.py")
    try:
        result = subprocess.run(
            [sys.executable, module],
            capture_output=True,
            text=True,
            env=env,
            timeout=3,
        )
        # If it exits cleanly (KeyboardInterrupt path), check warning
        assert "WinRT BLE will not be available" in result.stderr
    except subprocess.TimeoutExpired as exc:
        # Process is hanging in the scan loop — expected behavior on Linux.
        # The warning should appear in the partial stderr captured so far.
        partial_stderr = (exc.stderr or b"").decode("utf-8", errors="replace") if isinstance(exc.stderr, bytes) else (exc.stderr or "")
        assert "WinRT BLE will not be available" in partial_stderr, (
            f"Expected Linux/WSL warning in stderr before scan loop, got: {partial_stderr!r}"
        )


# ===========================================================================
# TASK 1: _atomic_write_credentials, _read_full_credentials, _parse_expiry_ms
# ===========================================================================

class TestAtomicWriteCredentials:
    """Tests for _atomic_write_credentials (T-mah-02: atomic write-back)."""

    def test_atomic_write_creates_file_with_exact_content(self, tmp_path):
        """_atomic_write_credentials writes the given object to path atomically."""
        from daemon.claude_usage_daemon_windows import _atomic_write_credentials
        creds = tmp_path / ".credentials.json"
        obj = {
            "claudeAiOauth": {
                "accessToken": "sk-ant-new-token",
                "refreshToken": "sk-ant-ort-new",
                "expiresAt": 9999999999000,
                "scopes": ["user:inference"],
                "subscriptionType": "claude_pro",
            }
        }
        _atomic_write_credentials(creds, obj)
        result = json.loads(creds.read_text(encoding="utf-8"))
        assert result == obj

    def test_atomic_write_preserves_unrelated_keys(self, tmp_path):
        """_atomic_write_credentials preserves subscriptionType and other unrelated keys through a write."""
        from daemon.claude_usage_daemon_windows import _atomic_write_credentials
        creds = tmp_path / ".credentials.json"
        # Write initial state with extra keys
        initial = {
            "claudeAiOauth": {
                "accessToken": "sk-ant-old",
                "refreshToken": "sk-ant-ort-old",
                "expiresAt": 1000,
                "scopes": ["user:inference", "user:profile"],
                "subscriptionType": "claude_pro",
                "rateLimitTier": "tier_1",
            }
        }
        creds.write_text(json.dumps(initial), encoding="utf-8")
        # Simulate mutating only the token-related keys
        updated = json.loads(creds.read_text(encoding="utf-8"))
        updated["claudeAiOauth"]["accessToken"] = "sk-ant-new"
        updated["claudeAiOauth"]["refreshToken"] = "sk-ant-ort-new"
        updated["claudeAiOauth"]["expiresAt"] = 9999999999000
        _atomic_write_credentials(creds, updated)
        result = json.loads(creds.read_text(encoding="utf-8"))
        # Unrelated keys must be preserved
        assert result["claudeAiOauth"]["subscriptionType"] == "claude_pro"
        assert result["claudeAiOauth"]["rateLimitTier"] == "tier_1"
        # Updated keys must be updated
        assert result["claudeAiOauth"]["accessToken"] == "sk-ant-new"
        assert result["claudeAiOauth"]["expiresAt"] == 9999999999000

    def test_atomic_write_no_tmp_file_leak_on_success(self, tmp_path):
        """No .cred-*.tmp sibling remains after a successful atomic write."""
        from daemon.claude_usage_daemon_windows import _atomic_write_credentials
        creds = tmp_path / ".credentials.json"
        obj = {"claudeAiOauth": {"accessToken": "sk-ant-tok"}}
        _atomic_write_credentials(creds, obj)
        tmp_files = list(tmp_path.glob(".cred-*.tmp"))
        assert tmp_files == [], f"Temp file leaked: {tmp_files}"

    def test_atomic_write_utf8_encoding(self, tmp_path):
        """_atomic_write_credentials uses UTF-8 encoding."""
        from daemon.claude_usage_daemon_windows import _atomic_write_credentials
        creds = tmp_path / ".credentials.json"
        obj = {"claudeAiOauth": {"accessToken": "sk-ant-tok", "note": "café"}}
        _atomic_write_credentials(creds, obj)
        raw = creds.read_bytes()
        # UTF-8 encoding of 'café' should be present
        assert "café".encode("utf-8") in raw


class TestReadFullCredentials:
    """Tests for _read_full_credentials (returns the whole object)."""

    def test_read_full_credentials_returns_nested_object(self, tmp_path, monkeypatch):
        """_read_full_credentials returns the full parsed dict including refreshToken."""
        from daemon.claude_usage_daemon_windows import _read_full_credentials
        creds = tmp_path / ".credentials.json"
        obj = {
            "claudeAiOauth": {
                "accessToken": "sk-ant-test-1234",
                "refreshToken": "sk-ant-ort-test-5678",
                "expiresAt": 9999999999000,
                "scopes": ["user:inference", "user:profile"],
                "subscriptionType": "claude_pro",
            }
        }
        creds.write_text(json.dumps(obj), encoding="utf-8")
        monkeypatch.setenv("CLAUDE_CREDENTIALS_PATH", str(creds))
        monkeypatch.delenv("CLAUDE_CONFIG_DIR", raising=False)
        result = _read_full_credentials()
        assert result is not None
        assert result["claudeAiOauth"]["refreshToken"] == "sk-ant-ort-test-5678"
        assert result["claudeAiOauth"]["subscriptionType"] == "claude_pro"

    def test_read_full_credentials_returns_none_when_no_file(self, tmp_path, monkeypatch):
        """_read_full_credentials returns None when no credential file exists."""
        from daemon.claude_usage_daemon_windows import _read_full_credentials
        monkeypatch.setenv("CLAUDE_CREDENTIALS_PATH", str(tmp_path / "missing.json"))
        monkeypatch.delenv("CLAUDE_CONFIG_DIR", raising=False)
        assert _read_full_credentials() is None

    def test_read_full_credentials_returns_none_on_invalid_json(self, tmp_path, monkeypatch):
        """_read_full_credentials returns None if file contains invalid JSON."""
        from daemon.claude_usage_daemon_windows import _read_full_credentials
        creds = tmp_path / ".credentials.json"
        creds.write_text("not json at all", encoding="utf-8")
        monkeypatch.setenv("CLAUDE_CREDENTIALS_PATH", str(creds))
        monkeypatch.delenv("CLAUDE_CONFIG_DIR", raising=False)
        assert _read_full_credentials() is None


class TestParseExpiryMs:
    """Tests for _parse_expiry_ms (factored expiry parser)."""

    def test_parse_expiry_ms_returns_int(self, tmp_path, monkeypatch):
        """_parse_expiry_ms returns the expiresAt int from a credentials object."""
        from daemon.claude_usage_daemon_windows import _parse_expiry_ms
        obj = {"claudeAiOauth": {"expiresAt": 9999999999000}}
        result = _parse_expiry_ms(obj)
        assert result == 9999999999000

    def test_parse_expiry_ms_returns_none_when_absent(self):
        """_parse_expiry_ms returns None when expiresAt is absent."""
        from daemon.claude_usage_daemon_windows import _parse_expiry_ms
        obj = {"claudeAiOauth": {}}
        assert _parse_expiry_ms(obj) is None

    def test_parse_expiry_ms_returns_none_for_non_numeric(self):
        """_parse_expiry_ms returns None when expiresAt is not numeric."""
        from daemon.claude_usage_daemon_windows import _parse_expiry_ms
        obj = {"claudeAiOauth": {"expiresAt": "not-a-number"}}
        assert _parse_expiry_ms(obj) is None

    def test_parse_expiry_ms_returns_none_for_no_oauth_key(self):
        """_parse_expiry_ms returns None when claudeAiOauth key is absent."""
        from daemon.claude_usage_daemon_windows import _parse_expiry_ms
        assert _parse_expiry_ms({}) is None

    def test_read_expiry_still_works_after_refactor(self, monkeypatch):
        """_read_expiry() contract is preserved after refactoring to use _parse_expiry_ms."""
        monkeypatch.setenv("CLAUDE_CREDENTIALS_PATH", str(FIXTURES / "credentials_nested.json"))
        monkeypatch.delenv("CLAUDE_CONFIG_DIR", raising=False)
        result = _read_expiry()
        assert result.startswith("2286-"), f"Expected year 2286, got: {result}"


# ===========================================================================
# TASK 2: _refresh_oauth_token + get_valid_token
# ===========================================================================

def _make_oauth_creds_file(tmp_path, access_token, refresh_token, expires_at_ms,
                            subscription_type="claude_pro", rate_limit_tier="tier_1"):
    """Helper: write a credentials file and return its path."""
    creds = tmp_path / ".credentials.json"
    obj = {
        "claudeAiOauth": {
            "accessToken": access_token,
            "refreshToken": refresh_token,
            "expiresAt": expires_at_ms,
            "scopes": ["user:inference", "user:profile"],
            "subscriptionType": subscription_type,
            "rateLimitTier": rate_limit_tier,
        }
    }
    creds.write_text(json.dumps(obj), encoding="utf-8")
    return creds


def _make_mock_http_client(status_code, response_json=None, side_effect=None):
    """Helper: build a mock httpx.AsyncClient for token endpoint calls."""
    resp = MagicMock()
    resp.status_code = status_code
    resp.json = MagicMock(return_value=response_json or {})
    resp.text = json.dumps(response_json or {})

    mock_client = AsyncMock()
    mock_client.__aenter__ = AsyncMock(return_value=mock_client)
    mock_client.__aexit__ = AsyncMock(return_value=False)
    if side_effect is not None:
        mock_client.post = AsyncMock(side_effect=side_effect)
    else:
        mock_client.post = AsyncMock(return_value=resp)
    return mock_client


class TestRefreshOauthToken:
    """Tests for _refresh_oauth_token (network call, fully mocked)."""

    def test_successful_refresh_returns_response_dict(self):
        """_refresh_oauth_token returns parsed dict on 200 response."""
        from daemon.claude_usage_daemon_windows import _refresh_oauth_token
        response_data = {
            "access_token": "sk-ant-oat01-new",
            "refresh_token": "sk-ant-ort01-new",
            "expires_in": 28800,
            "scope": "user:inference user:profile",
        }
        mock_client = _make_mock_http_client(200, response_data)
        with patch("httpx.AsyncClient", return_value=mock_client):
            result = _run(_refresh_oauth_token("sk-ant-ort01-old"))
        assert result["access_token"] == "sk-ant-oat01-new"
        assert result["refresh_token"] == "sk-ant-ort01-new"
        assert result["expires_in"] == 28800

    def test_400_raises_autherror(self):
        """_refresh_oauth_token raises AuthError on 400 (invalid_grant)."""
        from daemon.claude_usage_daemon_windows import _refresh_oauth_token
        mock_client = _make_mock_http_client(400, {"error": "invalid_grant"})
        with patch("httpx.AsyncClient", return_value=mock_client):
            with pytest.raises(AuthError):
                _run(_refresh_oauth_token("sk-ant-ort01-old"))

    def test_401_raises_autherror(self):
        """_refresh_oauth_token raises AuthError on 401."""
        from daemon.claude_usage_daemon_windows import _refresh_oauth_token
        mock_client = _make_mock_http_client(401, {"error": "unauthorized"})
        with patch("httpx.AsyncClient", return_value=mock_client):
            with pytest.raises(AuthError):
                _run(_refresh_oauth_token("sk-ant-ort01-old"))

    def test_403_raises_autherror(self):
        """_refresh_oauth_token raises AuthError on 403."""
        from daemon.claude_usage_daemon_windows import _refresh_oauth_token
        mock_client = _make_mock_http_client(403, {"error": "forbidden"})
        with patch("httpx.AsyncClient", return_value=mock_client):
            with pytest.raises(AuthError):
                _run(_refresh_oauth_token("sk-ant-ort01-old"))

    def test_network_error_returns_none(self):
        """_refresh_oauth_token returns None on network/transient error."""
        import httpx as _httpx
        from daemon.claude_usage_daemon_windows import _refresh_oauth_token
        mock_client = _make_mock_http_client(0, side_effect=_httpx.ConnectError("timeout"))
        with patch("httpx.AsyncClient", return_value=mock_client):
            result = _run(_refresh_oauth_token("sk-ant-ort01-old"))
        assert result is None

    def test_500_returns_none(self):
        """_refresh_oauth_token returns None on 5xx (transient)."""
        from daemon.claude_usage_daemon_windows import _refresh_oauth_token
        mock_client = _make_mock_http_client(503, {"error": "service_unavailable"})
        with patch("httpx.AsyncClient", return_value=mock_client):
            result = _run(_refresh_oauth_token("sk-ant-ort01-old"))
        assert result is None

    def test_uses_correct_client_id_and_grant_type(self):
        """_refresh_oauth_token sends the correct client_id and grant_type in request body."""
        from daemon.claude_usage_daemon_windows import (
            OAUTH_CLIENT_ID,
            _refresh_oauth_token,
        )
        response_data = {
            "access_token": "sk-ant-oat01-new",
            "expires_in": 28800,
        }
        mock_client = _make_mock_http_client(200, response_data)
        with patch("httpx.AsyncClient", return_value=mock_client):
            _run(_refresh_oauth_token("sk-ant-ort01-old"))
        call_kwargs = mock_client.post.call_args
        # The body should contain grant_type=refresh_token and the correct client_id
        body = call_kwargs[1].get("json") or call_kwargs[0][1] if len(call_kwargs[0]) > 1 else None
        if body is None and call_kwargs[1]:
            body = call_kwargs[1].get("json")
        assert body is not None, f"Could not find json body in call: {call_kwargs}"
        assert body["grant_type"] == "refresh_token"
        assert body["client_id"] == OAUTH_CLIENT_ID
        assert body["refresh_token"] == "sk-ant-ort01-old"


class TestGetValidToken:
    """Tests for get_valid_token (proactive + race re-read + write-back)."""

    def test_fresh_token_returned_without_network_call(self, tmp_path, monkeypatch):
        """get_valid_token returns access token immediately when not near expiry (no network)."""
        from daemon.claude_usage_daemon_windows import get_valid_token
        # expiresAt far in the future (year 2286)
        creds = _make_oauth_creds_file(tmp_path, "sk-ant-fresh", "sk-ant-ort-fresh", 9999999999000)
        monkeypatch.setenv("CLAUDE_CREDENTIALS_PATH", str(creds))
        monkeypatch.delenv("CLAUDE_CONFIG_DIR", raising=False)
        mock_client = AsyncMock()
        mock_client.__aenter__ = AsyncMock(return_value=mock_client)
        mock_client.__aexit__ = AsyncMock(return_value=False)
        mock_client.post = AsyncMock()
        with patch("httpx.AsyncClient", return_value=mock_client):
            result = _run(get_valid_token())
        assert result == "sk-ant-fresh"
        mock_client.post.assert_not_called()

    def test_proactive_refresh_on_near_expiry(self, tmp_path, monkeypatch):
        """get_valid_token refreshes proactively when token is near expiry (<5 min)."""
        from daemon.claude_usage_daemon_windows import get_valid_token
        # expiresAt 2 minutes in the future — inside the ~5 min proactive window
        near_expiry_ms = int(time.time() * 1000) + 2 * 60 * 1000
        creds = _make_oauth_creds_file(tmp_path, "sk-ant-old", "sk-ant-ort-old", near_expiry_ms)
        monkeypatch.setenv("CLAUDE_CREDENTIALS_PATH", str(creds))
        monkeypatch.delenv("CLAUDE_CONFIG_DIR", raising=False)
        new_expires_in = 28800
        response_data = {
            "access_token": "sk-ant-refreshed",
            "refresh_token": "sk-ant-ort-rotated",
            "expires_in": new_expires_in,
            "scope": "user:inference user:profile",
        }
        mock_client = _make_mock_http_client(200, response_data)
        with patch("httpx.AsyncClient", return_value=mock_client):
            result = _run(get_valid_token())
        assert result == "sk-ant-refreshed"
        # Verify write-back: credentials file should now have the new token
        written = json.loads(creds.read_text(encoding="utf-8"))
        assert written["claudeAiOauth"]["accessToken"] == "sk-ant-refreshed"
        assert written["claudeAiOauth"]["refreshToken"] == "sk-ant-ort-rotated"
        # expiresAt should be approximately now + 28800 * 1000 ms
        expected_expires = int(time.time() * 1000) + new_expires_in * 1000
        assert abs(written["claudeAiOauth"]["expiresAt"] - expected_expires) < 5000  # within 5s

    def test_rotated_refresh_token_persisted(self, tmp_path, monkeypatch):
        """get_valid_token persists the rotated refresh_token returned in 200 response."""
        from daemon.claude_usage_daemon_windows import get_valid_token
        near_expiry_ms = int(time.time() * 1000) + 60 * 1000  # 1 min — near expiry
        creds = _make_oauth_creds_file(tmp_path, "sk-ant-old", "sk-ant-ort-old", near_expiry_ms)
        monkeypatch.setenv("CLAUDE_CREDENTIALS_PATH", str(creds))
        monkeypatch.delenv("CLAUDE_CONFIG_DIR", raising=False)
        response_data = {
            "access_token": "sk-ant-new",
            "refresh_token": "sk-ant-ort-NEW-ROTATED",
            "expires_in": 28800,
        }
        mock_client = _make_mock_http_client(200, response_data)
        with patch("httpx.AsyncClient", return_value=mock_client):
            _run(get_valid_token())
        written = json.loads(creds.read_text(encoding="utf-8"))
        assert written["claudeAiOauth"]["refreshToken"] == "sk-ant-ort-NEW-ROTATED"

    def test_absent_refresh_token_in_response_reuses_old(self, tmp_path, monkeypatch):
        """When the 200 response omits refresh_token, the old refreshToken is preserved."""
        from daemon.claude_usage_daemon_windows import get_valid_token
        near_expiry_ms = int(time.time() * 1000) + 60 * 1000
        creds = _make_oauth_creds_file(tmp_path, "sk-ant-old", "sk-ant-ort-KEEP", near_expiry_ms)
        monkeypatch.setenv("CLAUDE_CREDENTIALS_PATH", str(creds))
        monkeypatch.delenv("CLAUDE_CONFIG_DIR", raising=False)
        # Response does NOT include refresh_token
        response_data = {"access_token": "sk-ant-new", "expires_in": 28800}
        mock_client = _make_mock_http_client(200, response_data)
        with patch("httpx.AsyncClient", return_value=mock_client):
            _run(get_valid_token())
        written = json.loads(creds.read_text(encoding="utf-8"))
        assert written["claudeAiOauth"]["refreshToken"] == "sk-ant-ort-KEEP"

    def test_write_back_preserves_subscription_type_and_other_keys(self, tmp_path, monkeypatch):
        """get_valid_token preserves subscriptionType, rateLimitTier, and other keys through refresh write-back."""
        from daemon.claude_usage_daemon_windows import get_valid_token
        near_expiry_ms = int(time.time() * 1000) + 60 * 1000
        creds = _make_oauth_creds_file(
            tmp_path, "sk-ant-old", "sk-ant-ort-old", near_expiry_ms,
            subscription_type="claude_max", rate_limit_tier="tier_2"
        )
        monkeypatch.setenv("CLAUDE_CREDENTIALS_PATH", str(creds))
        monkeypatch.delenv("CLAUDE_CONFIG_DIR", raising=False)
        response_data = {"access_token": "sk-ant-new", "refresh_token": "sk-ant-ort-new", "expires_in": 28800}
        mock_client = _make_mock_http_client(200, response_data)
        with patch("httpx.AsyncClient", return_value=mock_client):
            _run(get_valid_token())
        written = json.loads(creds.read_text(encoding="utf-8"))
        assert written["claudeAiOauth"]["subscriptionType"] == "claude_max"
        assert written["claudeAiOauth"]["rateLimitTier"] == "tier_2"

    def test_invalid_grant_raises_autherror(self, tmp_path, monkeypatch):
        """get_valid_token propagates AuthError when refresh endpoint returns 400."""
        from daemon.claude_usage_daemon_windows import get_valid_token
        near_expiry_ms = int(time.time() * 1000) + 60 * 1000
        creds = _make_oauth_creds_file(tmp_path, "sk-ant-old", "sk-ant-ort-old", near_expiry_ms)
        monkeypatch.setenv("CLAUDE_CREDENTIALS_PATH", str(creds))
        monkeypatch.delenv("CLAUDE_CONFIG_DIR", raising=False)
        mock_client = _make_mock_http_client(400, {"error": "invalid_grant"})
        with patch("httpx.AsyncClient", return_value=mock_client):
            with pytest.raises(AuthError):
                _run(get_valid_token())

    def test_transient_refresh_error_returns_none(self, tmp_path, monkeypatch):
        """get_valid_token returns None (no AuthError) when refresh has a transient network failure."""
        import httpx as _httpx
        from daemon.claude_usage_daemon_windows import get_valid_token
        near_expiry_ms = int(time.time() * 1000) + 60 * 1000
        creds = _make_oauth_creds_file(tmp_path, "sk-ant-old", "sk-ant-ort-old", near_expiry_ms)
        monkeypatch.setenv("CLAUDE_CREDENTIALS_PATH", str(creds))
        monkeypatch.delenv("CLAUDE_CONFIG_DIR", raising=False)
        mock_client = _make_mock_http_client(0, side_effect=_httpx.ConnectError("timeout"))
        with patch("httpx.AsyncClient", return_value=mock_client):
            result = _run(get_valid_token())
        assert result is None

    def test_no_raw_token_in_logs(self, tmp_path, monkeypatch, capsys):
        """get_valid_token never logs raw access or refresh token values (T-mah-01)."""
        from daemon.claude_usage_daemon_windows import get_valid_token
        near_expiry_ms = int(time.time() * 1000) + 60 * 1000
        secret_access = "sk-ant-SECRET-ACCESS-TOKEN-12345"
        secret_refresh = "sk-ant-ort-SECRET-REFRESH-TOKEN-67890"
        creds = _make_oauth_creds_file(tmp_path, secret_access, secret_refresh, near_expiry_ms)
        monkeypatch.setenv("CLAUDE_CREDENTIALS_PATH", str(creds))
        monkeypatch.delenv("CLAUDE_CONFIG_DIR", raising=False)
        response_data = {
            "access_token": "sk-ant-NEW-SECRET-ACCESS-ABCDEF",
            "refresh_token": "sk-ant-ort-NEW-SECRET-REFRESH-GHIJKL",
            "expires_in": 28800,
        }
        mock_client = _make_mock_http_client(200, response_data)
        with patch("httpx.AsyncClient", return_value=mock_client):
            _run(get_valid_token())
        captured = capsys.readouterr()
        combined = captured.out + captured.err
        # Neither old nor new token values should appear in any log output
        assert secret_access not in combined, "Old access token leaked to log (T-mah-01)"
        assert secret_refresh not in combined, "Old refresh token leaked to log (T-mah-01)"
        assert "sk-ant-NEW-SECRET-ACCESS-ABCDEF" not in combined, "New access token leaked to log (T-mah-01)"
        assert "sk-ant-ort-NEW-SECRET-REFRESH-GHIJKL" not in combined, "New refresh token leaked to log (T-mah-01)"


# ===========================================================================
# TASK 3: _poll_with_refresh + wiring in connect_and_run
# ===========================================================================

class TestPollWithRefresh:
    """Tests for _poll_with_refresh (reactive retry helper)."""

    def test_reactive_retry_succeeds_no_toast(self):
        """When poll_api raises AuthError then refresh succeeds, retry returns payload and no toast fires."""
        from daemon.claude_usage_daemon_windows import _poll_with_refresh

        good_payload = {"s": 42, "w": 10, "ok": True}

        # First poll raises AuthError, second succeeds
        poll_api_mock = AsyncMock(side_effect=[AuthError("401"), good_payload])
        # get_valid_token returns a fresh token on forced refresh
        get_valid_token_mock = AsyncMock(return_value="sk-ant-refreshed-token")

        tray_state = MagicMock()
        tray_state.set_error = MagicMock()

        with patch("daemon.claude_usage_daemon_windows.poll_api", poll_api_mock), \
             patch("daemon.claude_usage_daemon_windows.get_valid_token", get_valid_token_mock):
            result = _run(_poll_with_refresh("sk-ant-old-token", tray_state))

        assert result == good_payload
        tray_state.set_error.assert_not_called()

    def test_reactive_retry_forced_refresh_raises_autherror_fires_toast(self):
        """When forced refresh raises AuthError, toast IS fired and payload is None."""
        from daemon.claude_usage_daemon_windows import _poll_with_refresh

        # First poll raises AuthError; forced refresh also raises AuthError
        poll_api_mock = AsyncMock(side_effect=AuthError("401"))
        get_valid_token_mock = AsyncMock(side_effect=AuthError("refresh_failed"))

        tray_state = MagicMock()
        tray_state.set_error = MagicMock()

        with patch("daemon.claude_usage_daemon_windows.poll_api", poll_api_mock), \
             patch("daemon.claude_usage_daemon_windows.get_valid_token", get_valid_token_mock):
            result = _run(_poll_with_refresh("sk-ant-old-token", tray_state))

        assert result is None
        tray_state.set_error.assert_called_once()
        error_msg = tray_state.set_error.call_args[0][0]
        assert "claude login" in error_msg

    def test_reactive_retry_transient_refresh_returns_none_no_toast(self):
        """When forced refresh returns None (transient), payload is None and no toast fires."""
        from daemon.claude_usage_daemon_windows import _poll_with_refresh

        # First poll raises AuthError; forced refresh returns None (transient)
        poll_api_mock = AsyncMock(side_effect=AuthError("401"))
        get_valid_token_mock = AsyncMock(return_value=None)

        tray_state = MagicMock()
        tray_state.set_error = MagicMock()

        with patch("daemon.claude_usage_daemon_windows.poll_api", poll_api_mock), \
             patch("daemon.claude_usage_daemon_windows.get_valid_token", get_valid_token_mock):
            result = _run(_poll_with_refresh("sk-ant-old-token", tray_state))

        assert result is None
        tray_state.set_error.assert_not_called()

    def test_successful_first_poll_returns_payload_directly(self):
        """When poll_api succeeds on first try, payload returned without any refresh."""
        from daemon.claude_usage_daemon_windows import _poll_with_refresh

        good_payload = {"s": 50, "w": 20, "ok": True}
        poll_api_mock = AsyncMock(return_value=good_payload)
        get_valid_token_mock = AsyncMock()

        tray_state = MagicMock()

        with patch("daemon.claude_usage_daemon_windows.poll_api", poll_api_mock), \
             patch("daemon.claude_usage_daemon_windows.get_valid_token", get_valid_token_mock):
            result = _run(_poll_with_refresh("sk-ant-valid-token", tray_state))

        assert result == good_payload
        get_valid_token_mock.assert_not_called()


# ===========================================================================
# CODE-REVIEW FIX REGRESSIONS (260607-mah REVIEW.md: CR-01, WR-01, IN-01, IN-02)
# ===========================================================================

class TestReviewFixRegressions:
    """Regression guards for the issues found by code review on the OAuth refresh feature."""

    def test_cr01_proactive_autherror_does_not_crash_and_toasts(self):
        """CR-01: a genuine AuthError from the PROACTIVE get_valid_token inside
        connect_and_run must be caught — it toasts and the function returns
        normally instead of letting AuthError kill the daemon thread."""
        from daemon.claude_usage_daemon_windows import connect_and_run

        stop_event = asyncio.Event()

        # get_valid_token (proactive) raises AuthError; set stop so the loop exits
        # immediately after the except block (no TICK wait).
        def _raise_after_stopping(*_a, **_k):
            stop_event.set()
            raise AuthError("genuine refresh rejection")

        get_valid_token_mock = AsyncMock(side_effect=_raise_after_stopping)

        # Mock a successfully-connected BLE client.
        mock_client = AsyncMock()
        mock_client.is_connected = True
        mock_client.connect = AsyncMock()
        mock_client.disconnect = AsyncMock()
        mock_client.start_notify = AsyncMock()

        device = MagicMock()
        device.address = "AA:BB:CC:DD:EE:FF"

        tray_state = MagicMock()
        tray_state.set_error = MagicMock()

        with patch("daemon.claude_usage_daemon_windows.BleakClient", return_value=mock_client), \
             patch("daemon.claude_usage_daemon_windows.get_valid_token", get_valid_token_mock):
            # Must NOT raise AuthError — that would crash the daemon (CR-01).
            result = _run(connect_and_run(device, stop_event, tray_state))

        assert result is False  # no successful write occurred
        tray_state.set_error.assert_called_once()
        assert "claude login" in tray_state.set_error.call_args[0][0]

    def test_wr01_empty_access_token_in_200_is_transient_and_no_writeback(self, tmp_path, monkeypatch):
        """WR-01: a 200 response with an empty/absent access_token must be treated
        as transient (return None) and must NOT write back — writing an empty token
        would burn the single-use refresh token and spin a refresh loop."""
        from daemon.claude_usage_daemon_windows import get_valid_token
        near_expiry_ms = int(time.time() * 1000) + 60 * 1000  # near expiry -> would refresh
        creds = _make_oauth_creds_file(tmp_path, "sk-ant-old", "sk-ant-ort-PRECIOUS", near_expiry_ms)
        monkeypatch.setenv("CLAUDE_CREDENTIALS_PATH", str(creds))
        monkeypatch.delenv("CLAUDE_CONFIG_DIR", raising=False)
        # 200 but NO access_token field
        mock_client = _make_mock_http_client(200, {"refresh_token": "sk-ant-ort-rotated", "expires_in": 28800})
        with patch("httpx.AsyncClient", return_value=mock_client):
            result = _run(get_valid_token())
        assert result is None, "empty access_token must be treated as transient (None)"
        # The on-disk credentials must be UNCHANGED — refresh token not burned.
        written = json.loads(creds.read_text(encoding="utf-8"))
        assert written["claudeAiOauth"]["accessToken"] == "sk-ant-old"
        assert written["claudeAiOauth"]["refreshToken"] == "sk-ant-ort-PRECIOUS"

    def test_in01_race_reread_fresh_on_disk_skips_network(self, tmp_path, monkeypatch):
        """IN-01: if the FIRST read is stale but the race RE-READ finds a fresh
        token (Claude Code refreshed concurrently), get_valid_token returns the
        on-disk token and never spends the (possibly already-rotated) refresh token."""
        import daemon.claude_usage_daemon_windows as mod
        from daemon.claude_usage_daemon_windows import get_valid_token

        stale_ms = int(time.time() * 1000) + 60 * 1000           # within 5-min window -> "stale"
        fresh_ms = int(time.time() * 1000) + 9999999 * 1000       # far future -> "fresh"
        stale_obj = {"claudeAiOauth": {"accessToken": "sk-ant-stale", "refreshToken": "sk-ant-ort", "expiresAt": stale_ms}}
        fresh_obj = {"claudeAiOauth": {"accessToken": "sk-ant-CONCURRENT-FRESH", "refreshToken": "sk-ant-ort", "expiresAt": fresh_ms}}

        # Step 1 reads stale; the race re-read (with_path) returns the fresh on-disk token.
        first_read = MagicMock(return_value=stale_obj)
        race_read = MagicMock(return_value=(fresh_obj, tmp_path / ".credentials.json"))

        post_mock = AsyncMock()
        net_client = AsyncMock()
        net_client.__aenter__ = AsyncMock(return_value=net_client)
        net_client.__aexit__ = AsyncMock(return_value=False)
        net_client.post = post_mock

        with patch.object(mod, "_read_full_credentials", first_read), \
             patch.object(mod, "_read_full_credentials_with_path", race_read), \
             patch("httpx.AsyncClient", return_value=net_client):
            result = _run(get_valid_token())

        assert result == "sk-ant-CONCURRENT-FRESH"
        post_mock.assert_not_called()  # no network refresh — refresh token preserved

    def test_in02_404_on_primary_falls_back_to_secondary(self):
        """IN-02: a 404 on the primary OAuth URL escalates to the fallback URL."""
        from daemon.claude_usage_daemon_windows import (
            OAUTH_TOKEN_URL,
            OAUTH_FALLBACK_URL,
            _refresh_oauth_token,
        )
        resp404 = MagicMock()
        resp404.status_code = 404
        resp404.json = MagicMock(return_value={})
        resp404.text = "not found"
        resp200 = MagicMock()
        resp200.status_code = 200
        resp200.json = MagicMock(return_value={"access_token": "sk-ant-fallback", "expires_in": 28800})
        resp200.text = "ok"

        mock_client = AsyncMock()
        mock_client.__aenter__ = AsyncMock(return_value=mock_client)
        mock_client.__aexit__ = AsyncMock(return_value=False)
        mock_client.post = AsyncMock(side_effect=[resp404, resp200])

        with patch("httpx.AsyncClient", return_value=mock_client):
            result = _run(_refresh_oauth_token("sk-ant-ort-old"))

        assert result["access_token"] == "sk-ant-fallback"
        assert mock_client.post.call_count == 2
        # First attempt hit the primary URL, second hit the fallback.
        first_url = mock_client.post.call_args_list[0][0][0]
        second_url = mock_client.post.call_args_list[1][0][0]
        assert first_url == OAUTH_TOKEN_URL
        assert second_url == OAUTH_FALLBACK_URL
