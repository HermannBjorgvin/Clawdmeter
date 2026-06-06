#!/usr/bin/env python3
"""Unit tests for the Windows daemon's OAuth self-refresh logic.

Mirrors daemon/tests/test_macos_refresh.py. Covers the pure, side-effect-free
helpers that back token renewal:
  - _credentials_expiry_seconds  (expiresAt ms -> seconds)
  - _apply_refresh_response      (OAuth 200 body -> stored credential dict)

Plus the Windows-specific write-back path-targeting, exercised against a plain
tmp_path file (Windows keeps .credentials.json as plaintext, so a temp file IS
the realistic store — no DPAPI, no Keychain, no network).

The actual OAuth POST is mocked; nothing here touches the live endpoint or any
OS credential store.

Run: python -m pytest daemon/tests/test_windows_refresh.py -x -q
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

from daemon.claude_usage_daemon_windows import (  # noqa: E402
    _apply_refresh_response,
    _credentials_expiry_seconds,
    _extract_access_token,
    _read_credentials_blob,
    _write_credentials_blob,
    refresh_access_token,
)

FIXTURES = Path(__file__).parent / "fixtures"


@pytest.fixture(autouse=True)
def _reset_refresh_cooldown():
    """Clear the module-level refresh cooldown/backoff state before each test, so
    the refresh-helper tests don't skip one another via the shared timestamp."""
    import daemon.claude_usage_daemon_windows as mod
    mod._last_refresh_attempt = 0.0
    mod._refresh_failures = 0
    yield


# --- _credentials_expiry_seconds --------------------------------------------

def test_expiry_milliseconds_normalized_to_seconds():
    raw = (FIXTURES / "credentials_nested.json").read_text()
    secs = _credentials_expiry_seconds(raw)
    assert secs == 9999999999.0  # 9999999999000 ms -> seconds


def test_expiry_direct_shape_supported():
    """A top-level (non-nested) credential dict still yields its expiry."""
    raw = json.dumps({"accessToken": "x", "expiresAt": 9999999999000})
    assert _credentials_expiry_seconds(raw) == 9999999999.0


def test_expiry_seconds_value_passed_through():
    """A value already in seconds (< 1e12) is returned unchanged, not divided."""
    raw = json.dumps({"claudeAiOauth": {"expiresAt": 1700000000}})
    assert _credentials_expiry_seconds(raw) == 1700000000.0


def test_expiry_missing_returns_none():
    assert _credentials_expiry_seconds('{"claudeAiOauth": {}}') is None


def test_expiry_bad_blob_returns_none():
    # A non-None argument is treated as the blob itself (None is the sentinel
    # for "read the live store", so it's deliberately not tested here).
    assert _credentials_expiry_seconds("not json") is None
    assert _credentials_expiry_seconds("") is None


def test_expiry_non_dict_json_returns_none():
    # WR-01 parity: non-dict top-level JSON must not crash.
    assert _credentials_expiry_seconds("[1, 2, 3]") is None


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
    """The mutated dict must serialize to single-line JSON (parity with the
    macOS test — keeps the written file clean and round-trippable)."""
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


# --- write-back path-targeting (plain-file store, no OS keystore) -----------

def test_read_credentials_blob_targets_first_hit(tmp_path, monkeypatch):
    """_read_credentials_blob returns the first readable file's (path, blob)."""
    missing = tmp_path / "missing" / ".credentials.json"
    present = tmp_path / "present" / ".credentials.json"
    present.parent.mkdir(parents=True)
    present.write_text('{"accessToken": "x"}', encoding="utf-8")
    import daemon.claude_usage_daemon_windows as mod
    monkeypatch.setattr(mod, "_windows_credential_candidates", lambda: [missing, present])
    found = _read_credentials_blob()
    assert found is not None
    path, blob = found
    assert path == present
    assert json.loads(blob)["accessToken"] == "x"


def test_read_credentials_blob_none_when_absent(tmp_path, monkeypatch):
    import daemon.claude_usage_daemon_windows as mod
    monkeypatch.setattr(
        mod, "_windows_credential_candidates", lambda: [tmp_path / "nope.json"]
    )
    assert _read_credentials_blob() is None


def test_write_credentials_blob_round_trips(tmp_path):
    target = tmp_path / "sub" / ".credentials.json"  # parent created on demand
    assert _write_credentials_blob(target, '{"a": 1}') is True
    assert json.loads(target.read_text(encoding="utf-8")) == {"a": 1}


def test_write_credentials_blob_failure_is_nonfatal(tmp_path):
    """A write to an unwritable target returns False rather than raising."""
    # A path whose "parent" is actually a file can't be mkdir'd -> OSError -> False.
    blocker = tmp_path / "blocker"
    blocker.write_text("x")
    target = blocker / "child" / ".credentials.json"
    assert _write_credentials_blob(target, "{}") is False


# --- refresh_access_token end-to-end (httpx mocked, plain-file store) -------
#
# These drive an async function, but we deliberately wrap each call in
# asyncio.run() from a SYNC test rather than reach for pytest-asyncio (not a
# dependency) or the asyncio.get_event_loop() pattern that breaks the existing
# test_windows_poll.py under Python 3.14. asyncio.run() spins a fresh loop and
# works cleanly on 3.14.

def _mock_httpx(status_code, json_body=None):
    """Patch httpx.AsyncClient so the refresh POST returns a canned response.

    Returns (patch_ctx, client) so a test can both install the patch and later
    assert whether the POST was actually awaited.
    """
    resp = MagicMock()
    resp.status_code = status_code
    resp.text = "mocked"
    resp.json = MagicMock(return_value=json_body or {})
    client = AsyncMock()
    client.__aenter__ = AsyncMock(return_value=client)
    client.__aexit__ = AsyncMock(return_value=False)
    client.post = AsyncMock(return_value=resp)
    return patch("httpx.AsyncClient", return_value=client), client


def _point_at(tmp_path, monkeypatch, blob):
    creds = tmp_path / ".credentials.json"
    creds.write_text(blob, encoding="utf-8")
    import daemon.claude_usage_daemon_windows as mod
    monkeypatch.setattr(mod, "_windows_credential_candidates", lambda: [creds])
    return creds


def test_refresh_writes_back_to_same_file(tmp_path, monkeypatch):
    """A 200 refresh rotates the tokens and writes them back to the file the
    daemon read from — the location Claude Code also reads."""
    creds = _point_at(tmp_path, monkeypatch, json.dumps({
        "claudeAiOauth": {
            "accessToken": "OLD", "refreshToken": "OLD_R",
            "expiresAt": 1, "scopes": ["user:inference"],
        }
    }))
    ctx, _ = _mock_httpx(200, {"access_token": "NEW", "refresh_token": "NEW_R", "expires_in": 28800})
    with ctx:
        new = asyncio.run(refresh_access_token())
    assert new == "NEW"
    stored = json.loads(creds.read_text(encoding="utf-8"))["claudeAiOauth"]
    assert stored["accessToken"] == "NEW"
    assert stored["refreshToken"] == "NEW_R"
    assert stored["scopes"] == ["user:inference"]  # untouched
    assert stored["expiresAt"] > 1  # rotated forward


def test_refresh_non_200_writes_nothing(tmp_path, monkeypatch):
    """On any non-200 the stored credentials are left exactly as they were."""
    original = json.dumps({"claudeAiOauth": {"accessToken": "OLD", "refreshToken": "R", "expiresAt": 1}})
    creds = _point_at(tmp_path, monkeypatch, original)
    ctx, _ = _mock_httpx(400, {"error": "invalid_grant"})
    with ctx:
        new = asyncio.run(refresh_access_token())
    assert new is None
    assert creds.read_text(encoding="utf-8") == original  # byte-for-byte untouched


def test_refresh_preserves_refresh_token_when_not_rotated(tmp_path, monkeypatch):
    """A 200 that omits refresh_token keeps the stored one (only access rotates)."""
    creds = _point_at(tmp_path, monkeypatch, json.dumps(
        {"claudeAiOauth": {"accessToken": "OLD", "refreshToken": "KEEP", "expiresAt": 1}}))
    ctx, _ = _mock_httpx(200, {"access_token": "NEW", "expires_in": 60})
    with ctx:
        new = asyncio.run(refresh_access_token())
    assert new == "NEW"
    stored = json.loads(creds.read_text(encoding="utf-8"))["claudeAiOauth"]
    assert stored["accessToken"] == "NEW"
    assert stored["refreshToken"] == "KEEP"


def test_refresh_no_refresh_token_is_noop(tmp_path, monkeypatch):
    """Without a refresh token there is nothing to exchange; no POST, no write."""
    original = json.dumps({"claudeAiOauth": {"accessToken": "OLD", "expiresAt": 1}})
    creds = _point_at(tmp_path, monkeypatch, original)
    ctx, client = _mock_httpx(200, {"access_token": "NEW"})
    with ctx:
        new = asyncio.run(refresh_access_token())
    assert new is None
    assert creds.read_text(encoding="utf-8") == original
    client.post.assert_not_awaited()  # never even hit the network


def test_refresh_no_credentials_returns_none(tmp_path, monkeypatch):
    import daemon.claude_usage_daemon_windows as mod
    monkeypatch.setattr(
        mod, "_windows_credential_candidates", lambda: [tmp_path / "nope.json"]
    )
    assert asyncio.run(refresh_access_token()) is None


def test_refresh_cooldown_blocks_rapid_reattempts(tmp_path, monkeypatch):
    """Two refresh calls within REFRESH_COOLDOWN: the second is skipped with no
    network call — the guard that stops a data-hungry device from hammering the
    OAuth endpoint into a 429 loop (parity with the macOS daemon)."""
    _point_at(tmp_path, monkeypatch, json.dumps(
        {"claudeAiOauth": {"accessToken": "OLD", "refreshToken": "R", "expiresAt": 1}}))
    ctx, client = _mock_httpx(200, {"access_token": "NEW", "expires_in": 28800})
    with ctx:
        first = asyncio.run(refresh_access_token())
        second = asyncio.run(refresh_access_token())
    assert first == "NEW"
    assert second is None
    client.post.assert_awaited_once()  # one network call despite two refresh calls


def test_refresh_backoff_grows_and_skips_within_window(tmp_path, monkeypatch):
    """A failed (429) refresh increments the failure counter; the next call within
    the now-doubled window is skipped without a second network call."""
    import daemon.claude_usage_daemon_windows as mod
    _point_at(tmp_path, monkeypatch, json.dumps(
        {"claudeAiOauth": {"accessToken": "OLD", "refreshToken": "R", "expiresAt": 1}}))
    ctx, client = _mock_httpx(429, {})
    with ctx:
        first = asyncio.run(refresh_access_token())    # fires -> 429 -> failure
        client.post.reset_mock()
        second = asyncio.run(refresh_access_token())   # within backoff -> skipped
    assert first is None
    assert second is None
    assert mod._refresh_failures == 1
    client.post.assert_not_awaited()                   # no second hit on the endpoint


def test_refresh_success_resets_backoff(tmp_path, monkeypatch):
    """A 200 clears the accumulated failure count so the window returns to base."""
    import daemon.claude_usage_daemon_windows as mod
    _point_at(tmp_path, monkeypatch, json.dumps(
        {"claudeAiOauth": {"accessToken": "OLD", "refreshToken": "R", "expiresAt": 1}}))
    monkeypatch.setattr(mod, "_refresh_failures", 3)   # pretend we've been failing a while
    ctx, _ = _mock_httpx(200, {"access_token": "NEW", "expires_in": 28800})
    with ctx:
        new = asyncio.run(refresh_access_token())
    assert new == "NEW"
    assert mod._refresh_failures == 0                  # success cleared the backoff


# --- connect_and_run wiring: free-ride (no proactive refresh) + on-401 retry --
#
# These drive the REAL connect_and_run to prove the loop free-rides on Claude
# Code's token (never refreshing proactively) and only refreshes reactively when
# a poll 401s. The refresh helper itself is mocked here — its own behavior is
# covered above — so the assertions focus on the loop's control flow. Each test
# overrides the conftest autouse defaults inside its own patch() block.


def _connected_client():
    """A BleakClient mock that connects, stays connected, and accepts writes."""
    client = AsyncMock()
    client.connect = AsyncMock(return_value=None)
    client.is_connected = True
    client.disconnect = AsyncMock()
    client.start_notify = AsyncMock()
    client.write_gatt_char = AsyncMock(return_value=None)
    return client


def test_connect_no_proactive_refresh_uses_token_as_is():
    """Free-ride: a still-valid (even near-expiry) token is used AS-IS — the loop does
    NOT refresh before polling. Refresh happens only reactively (on a 401), so the
    daemon never competes with the app's own refreshes / trips the token endpoint."""
    import daemon.claude_usage_daemon_windows as mod

    device = MagicMock()
    device.address = "AA:BB:CC:DD:EE:FF"
    client = _connected_client()
    refresh = AsyncMock(return_value="REFRESHED")
    used_tokens = []

    async def go():
        stop_event = asyncio.Event()

        async def fake_poll(token):
            used_tokens.append(token)
            stop_event.set()  # one poll, then the loop exits
            return {"ok": True}

        with patch.object(mod, "BleakClient", return_value=client), \
             patch.object(mod, "read_token", return_value="STALE"), \
             patch.object(mod, "_credentials_expiry_seconds", return_value=time.time()), \
             patch.object(mod, "refresh_access_token", refresh), \
             patch.object(mod, "poll_api", new=fake_poll):
            await mod.connect_and_run(device, stop_event)

    asyncio.run(go())
    refresh.assert_not_awaited()         # no proactive refresh — token used as-is
    assert used_tokens == ["STALE"]      # polled with the token Claude Code currently holds


def test_connect_auth_error_refreshes_and_retries_then_recovers():
    """A 401/403 triggers a single refresh-and-retry; on success the device gets
    its payload and the tray is NOT flipped to the actionable error state."""
    import daemon.claude_usage_daemon_windows as mod

    device = MagicMock()
    device.address = "AA:BB:CC:DD:EE:FF"
    client = _connected_client()
    tray = MagicMock()
    refresh = AsyncMock(return_value="NEW")
    poll_tokens = []

    async def go():
        stop_event = asyncio.Event()

        async def fake_poll(token):
            poll_tokens.append(token)
            if len(poll_tokens) == 1:
                raise mod.AuthError(401)  # first poll: token rejected
            stop_event.set()
            return {"ok": True}  # retry with refreshed token succeeds

        with patch.object(mod, "BleakClient", return_value=client), \
             patch.object(mod, "read_token", return_value="OLD"), \
             patch.object(mod, "_credentials_expiry_seconds", return_value=None), \
             patch.object(mod, "refresh_access_token", refresh), \
             patch.object(mod, "poll_api", new=fake_poll):
            await mod.connect_and_run(device, stop_event, tray)

    asyncio.run(go())
    refresh.assert_awaited_once()
    assert poll_tokens == ["OLD", "NEW"]  # retried with the refreshed token
    tray.set_error.assert_not_called()  # recovered → no "run claude login" toast
    tray.set_connected.assert_called()  # device received fresh data
    client.write_gatt_char.assert_awaited()  # payload actually written


def test_connect_auth_error_refresh_fails_sets_error():
    """When the refresh-and-retry cannot recover (refresh returns None), the loop
    falls through to the actionable 'token expired — run claude login' toast."""
    import daemon.claude_usage_daemon_windows as mod

    device = MagicMock()
    device.address = "AA:BB:CC:DD:EE:FF"
    client = _connected_client()
    tray = MagicMock()
    refresh = AsyncMock(return_value=None)  # refresh unavailable / failed

    async def go():
        stop_event = asyncio.Event()

        async def fake_poll(_token):
            stop_event.set()
            raise mod.AuthError(401)

        with patch.object(mod, "BleakClient", return_value=client), \
             patch.object(mod, "read_token", return_value="OLD"), \
             patch.object(mod, "_credentials_expiry_seconds", return_value=None), \
             patch.object(mod, "refresh_access_token", refresh), \
             patch.object(mod, "poll_api", new=fake_poll):
            await mod.connect_and_run(device, stop_event, tray)

    asyncio.run(go())
    refresh.assert_awaited_once()
    tray.set_error.assert_called_once_with("token expired — run claude login")


def test_note_poll_success_clears_backoff():
    """A successful POLL clears the refresh-failure backoff (parity with macOS). Under
    free-ride the daemon recovers via Claude Code's own refresh / a re-login, not its
    own refresh, so without this the counter stays pinned and blocks the next reactive
    refresh that might have succeeded."""
    import daemon.claude_usage_daemon_windows as mod
    mod._refresh_failures = 9
    mod._last_refresh_attempt = 123456.0
    mod._note_poll_success()
    assert mod._refresh_failures == 0
    assert mod._last_refresh_attempt == 0.0


def test_note_poll_success_noop_when_already_clear():
    """No-op when there's no failure streak to clear (avoids a spurious log line)."""
    import daemon.claude_usage_daemon_windows as mod
    mod._refresh_failures = 0
    mod._last_refresh_attempt = 0.0
    mod._note_poll_success()
    assert mod._refresh_failures == 0
    assert mod._last_refresh_attempt == 0.0
