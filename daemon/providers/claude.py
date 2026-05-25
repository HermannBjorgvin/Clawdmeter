"""Claude Code usage provider."""

from __future__ import annotations

import getpass
import json
import re
import subprocess
import sys
import time
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

import httpx

from . import Credentials, Usage

KEYCHAIN_SERVICE = "Claude Code-credentials"
CREDENTIALS_PATH = Path.home() / ".claude" / ".credentials.json"

API_URL = "https://api.anthropic.com/v1/messages"
API_HEADERS_TEMPLATE = {
    "anthropic-version": "2023-06-01",
    "anthropic-beta": "oauth-2025-04-20",
    "Content-Type": "application/json",
    "User-Agent": "claude-code/2.1.5",
}
API_BODY = {
    "model": "claude-haiku-4-5-20251001",
    "max_tokens": 1,
    "messages": [{"role": "user", "content": "hi"}],
}

OAUTH_TOKEN_URL = "https://platform.claude.com/v1/oauth/token"
OAUTH_CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e"
OAUTH_SCOPE = " ".join(
    [
        "user:profile",
        "user:inference",
        "user:sessions:claude_code",
        "user:mcp_servers",
        "user:file_upload",
    ]
)


@dataclass
class _AuthPayload:
    storage: str
    blob: str
    data: dict | None


class ClaudeProvider:
    name = "claude"

    def __init__(self, log: Callable[[str], None]) -> None:
        self._log = log
        self._auth_payload: _AuthPayload | None = None

    def read_credentials(self) -> Credentials | None:
        auth_payload = (
            self._read_auth_keychain()
            if sys.platform == "darwin"
            else self._read_auth_file()
        )
        if auth_payload is None and sys.platform == "darwin":
            auth_payload = self._read_auth_file()
        if auth_payload is None:
            return None
        credentials = self._extract_credentials(auth_payload)
        if credentials is None:
            return None
        self._auth_payload = auth_payload
        return credentials

    async def fetch_usage(self) -> Usage | None:
        credentials = self.read_credentials()
        if credentials is None:
            self._log("No token; skipping poll")
            return None
        return await self._poll_api(credentials)

    @staticmethod
    def _parse_blob(blob: str) -> dict | None:
        blob = blob.strip()
        if not blob:
            return None
        try:
            parsed = json.loads(blob)
        except json.JSONDecodeError:
            return None
        return parsed if isinstance(parsed, dict) else None

    def _extract_credentials(self, auth_payload: _AuthPayload) -> Credentials | None:
        """Pull OAuth tokens out of a Claude Code credentials blob."""
        data = auth_payload.data
        if isinstance(data, dict):
            oauth = data.get("claudeAiOauth")
            if not isinstance(oauth, dict):
                oauth = data
            access_token = oauth.get("accessToken")
            refresh_token = oauth.get("refreshToken")
            if isinstance(access_token, str):
                return Credentials(
                    access_token=access_token,
                    refresh_token=refresh_token if isinstance(refresh_token, str) else None,
                    storage=auth_payload.storage,
                )
            for v in data.values():
                if isinstance(v, dict) and isinstance(v.get("accessToken"), str):
                    refresh_token = v.get("refreshToken")
                    return Credentials(
                        access_token=v["accessToken"],
                        refresh_token=refresh_token
                        if isinstance(refresh_token, str)
                        else None,
                        storage=auth_payload.storage,
                    )
        blob = auth_payload.blob.strip()
        m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
        if m:
            refresh_match = re.search(r'"refreshToken"\s*:\s*"([^"]+)"', blob)
            return Credentials(
                access_token=m.group(1),
                refresh_token=refresh_match.group(1) if refresh_match else None,
                storage=auth_payload.storage,
            )
        if re.fullmatch(r"[A-Za-z0-9_\-.~+/=]{20,}", blob):
            return Credentials(access_token=blob, storage=auth_payload.storage)
        return None

    def _read_auth_keychain(self) -> _AuthPayload | None:
        commands = [
            [
                "security",
                "find-generic-password",
                "-s",
                KEYCHAIN_SERVICE,
                "-a",
                getpass.getuser(),
                "-w",
            ],
            [
                "security",
                "find-generic-password",
                "-s",
                KEYCHAIN_SERVICE,
                "-w",
            ],
        ]
        for command in commands:
            try:
                out = subprocess.run(
                    command,
                    check=True,
                    capture_output=True,
                    text=True,
                    timeout=10,
                )
            except subprocess.CalledProcessError as e:
                last_error = f"Keychain read failed (rc={e.returncode}): {e.stderr.strip()}"
                continue
            except (FileNotFoundError, subprocess.TimeoutExpired) as e:
                self._log(f"Keychain access error: {e}")
                return None
            blob = out.stdout.strip()
            return _AuthPayload("keychain", blob, self._parse_blob(blob))
        self._log(last_error)
        return None

    def _write_auth_keychain(self, blob: str) -> bool:
        try:
            subprocess.run(
                [
                    "security",
                    "add-generic-password",
                    "-U",
                    "-s",
                    KEYCHAIN_SERVICE,
                    "-a",
                    getpass.getuser(),
                    "-w",
                    blob,
                ],
                check=True,
                capture_output=True,
                text=True,
                timeout=10,
            )
        except subprocess.CalledProcessError as e:
            self._log(f"Keychain write failed (rc={e.returncode}): {e.stderr.strip()}")
            return False
        except (FileNotFoundError, subprocess.TimeoutExpired) as e:
            self._log(f"Keychain access error: {e}")
            return False
        return True

    def _read_auth_file(self) -> _AuthPayload | None:
        try:
            raw = CREDENTIALS_PATH.read_text()
        except OSError as e:
            self._log(f"Error reading credentials: {e}")
            return None
        return _AuthPayload("file", raw, self._parse_blob(raw))

    def _write_auth_file(self, blob: str) -> bool:
        try:
            CREDENTIALS_PATH.parent.mkdir(parents=True, exist_ok=True)
            CREDENTIALS_PATH.write_text(blob)
            CREDENTIALS_PATH.chmod(0o600)
        except OSError as e:
            self._log(f"Error writing credentials: {e}")
            return False
        return True

    def _persist_refreshed_credentials(
        self, credentials: Credentials, refresh_data: dict
    ) -> Credentials | None:
        access_token = refresh_data.get("access_token")
        refresh_token = refresh_data.get("refresh_token") or credentials.refresh_token
        if not isinstance(access_token, str):
            self._log("OAuth refresh response missing access token")
            return None

        auth_payload = self._auth_payload
        if auth_payload is None or auth_payload.data is None:
            self._log("OAuth refresh succeeded but credential storage is not writable")
            return Credentials(
                access_token=access_token,
                refresh_token=refresh_token if isinstance(refresh_token, str) else None,
                storage=credentials.storage,
            )

        data = dict(auth_payload.data)
        oauth = data.get("claudeAiOauth")
        if not isinstance(oauth, dict):
            oauth = data
        oauth["accessToken"] = access_token
        if isinstance(refresh_token, str):
            oauth["refreshToken"] = refresh_token
        expires_in = refresh_data.get("expires_in")
        if isinstance(expires_in, (int, float)):
            oauth["expiresAt"] = int(time.time() * 1000 + expires_in * 1000)
        scopes = refresh_data.get("scope")
        if isinstance(scopes, str):
            oauth["scopes"] = scopes.split()

        blob = json.dumps(data, indent=2)
        if auth_payload.storage == "keychain":
            ok = self._write_auth_keychain(blob)
        else:
            ok = self._write_auth_file(blob)
        if not ok:
            return None

        self._auth_payload = _AuthPayload(auth_payload.storage, blob, data)
        return Credentials(
            access_token=access_token,
            refresh_token=refresh_token if isinstance(refresh_token, str) else None,
            storage=credentials.storage,
        )

    async def _refresh_credentials(self, credentials: Credentials) -> Credentials | None:
        if not credentials.refresh_token:
            self._log("API HTTP 401 and no Claude refresh token is available")
            return None
        body = {
            "grant_type": "refresh_token",
            "refresh_token": credentials.refresh_token,
            "client_id": OAUTH_CLIENT_ID,
            "scope": OAUTH_SCOPE,
        }
        try:
            async with httpx.AsyncClient(timeout=30.0) as http:
                resp = await http.post(
                    OAUTH_TOKEN_URL,
                    headers={"Content-Type": "application/json"},
                    json=body,
                )
        except httpx.HTTPError as e:
            self._log(f"OAuth refresh failed: {e}")
            return None
        if resp.status_code >= 400:
            self._log(f"OAuth refresh HTTP {resp.status_code}")
            return None
        try:
            refresh_data = resp.json()
        except json.JSONDecodeError:
            self._log("OAuth refresh response was not JSON")
            return None
        refreshed = self._persist_refreshed_credentials(credentials, refresh_data)
        if refreshed is not None:
            self._log("OAuth refresh succeeded")
        return refreshed

    async def _poll_api(self, credentials: Credentials, retry_on_401: bool = True) -> Usage | None:
        headers = dict(API_HEADERS_TEMPLATE)
        headers["Authorization"] = f"Bearer {credentials.access_token}"
        try:
            async with httpx.AsyncClient(timeout=20.0) as http:
                resp = await http.post(API_URL, headers=headers, json=API_BODY)
        except httpx.HTTPError as e:
            self._log(f"API call failed: {e}")
            return None
        if resp.status_code == 401 and retry_on_401:
            self._log("API HTTP 401; attempting OAuth refresh")
            refreshed = await self._refresh_credentials(credentials)
            if refreshed is None:
                return None
            return await self._poll_api(refreshed, retry_on_401=False)
        if resp.status_code >= 400:
            self._log(f"API HTTP {resp.status_code}: {resp.text[:200]}")
            return None

        def hdr(name: str, default: str = "0") -> str:
            return resp.headers.get(name, default)

        now = time.time()

        def reset_minutes(reset_ts: str) -> int:
            try:
                r = float(reset_ts)
            except ValueError:
                return 0
            mins = (r - now) / 60.0
            return int(round(mins)) if mins > 0 else 0

        def pct(util: str) -> int:
            try:
                return int(round(float(util) * 100))
            except ValueError:
                return 0

        return Usage(
            session_pct=pct(hdr("anthropic-ratelimit-unified-5h-utilization")),
            session_reset_min=reset_minutes(hdr("anthropic-ratelimit-unified-5h-reset")),
            weekly_pct=pct(hdr("anthropic-ratelimit-unified-7d-utilization")),
            weekly_reset_min=reset_minutes(hdr("anthropic-ratelimit-unified-7d-reset")),
            status=hdr("anthropic-ratelimit-unified-5h-status", "unknown"),
            ok=True,
        )
