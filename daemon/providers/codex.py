"""Codex ChatGPT usage provider."""

from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import time
from collections.abc import Callable
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

import httpx

from . import Credentials, Usage

KEYCHAIN_SERVICE = "Codex Auth"
REFRESH_TOKEN_URL = "https://auth.openai.com/oauth/token"
REFRESH_CLIENT_ID = "app_EMoamEEZ73f0CkXaXp7hrann"
USAGE_URLS = (
    "https://chatgpt.com/backend-api/wham/usage",
    "https://chatgpt.com/backend-api/codex/usage",
)


@dataclass
class _AuthPayload:
    storage: str
    data: dict[str, Any]
    path: Path | None = None
    keychain_account: str | None = None


class CodexProvider:
    name = "codex"

    def __init__(self, log: Callable[[str], None]) -> None:
        self._log = log
        self._auth_payload: _AuthPayload | None = None
        self._auth_error: str | None = None

    def read_credentials(self) -> Credentials | None:
        self._auth_payload = None
        self._auth_error = None

        auth_payload = self._read_file_auth()
        if auth_payload is None and sys.platform == "darwin":
            auth_payload = self._read_keychain_auth()
        if auth_payload is None:
            self._auth_error = self._auth_error or "codex_no_auth"
            return None

        credentials = self._credentials_from_payload(auth_payload)
        if credentials is None:
            return None
        self._auth_payload = auth_payload
        return credentials

    async def fetch_usage(self) -> Usage | None:
        credentials = self.read_credentials()
        if credentials is None:
            status = self._auth_error or "codex_no_auth"
            self._log(f"Codex credentials unavailable: {status}")
            return Usage(0, 0, 0, 0, status, False)
        return await self._poll_usage(credentials)

    @staticmethod
    def _codex_home() -> Path:
        return Path(os.environ.get("CODEX_HOME", "~/.codex")).expanduser()

    def _read_file_auth(self) -> _AuthPayload | None:
        auth_path = self._codex_home() / "auth.json"
        try:
            raw = auth_path.read_text()
        except FileNotFoundError:
            return None
        except OSError as e:
            self._log(f"Codex auth.json read failed: {e}")
            return None
        try:
            data = json.loads(raw)
        except json.JSONDecodeError as e:
            self._log(f"Codex auth.json parse failed: {e}")
            return None
        if not isinstance(data, dict):
            self._log("Codex auth.json root is not an object")
            return None
        return _AuthPayload("file", data, path=auth_path)

    def _read_keychain_auth(self) -> _AuthPayload | None:
        account = self._keychain_account()
        try:
            out = subprocess.run(
                [
                    "security",
                    "find-generic-password",
                    "-s",
                    KEYCHAIN_SERVICE,
                    "-a",
                    account,
                    "-w",
                ],
                check=True,
                capture_output=True,
                text=True,
                timeout=10,
            )
        except subprocess.CalledProcessError:
            return None
        except (FileNotFoundError, subprocess.TimeoutExpired) as e:
            self._log(f"Codex keychain access failed: {e}")
            return None
        try:
            data = json.loads(out.stdout)
        except json.JSONDecodeError as e:
            self._log(f"Codex keychain auth parse failed: {e}")
            return None
        if not isinstance(data, dict):
            self._log("Codex keychain auth root is not an object")
            return None
        return _AuthPayload("keychain", data, keychain_account=account)

    def _credentials_from_payload(self, auth_payload: _AuthPayload) -> Credentials | None:
        data = auth_payload.data
        tokens = data.get("tokens")
        if isinstance(tokens, dict):
            access_token = tokens.get("access_token")
            refresh_token = tokens.get("refresh_token")
            account_id = tokens.get("account_id") or self._account_id_from_id_token(
                tokens.get("id_token")
            )
            if isinstance(access_token, str) and access_token:
                return Credentials(
                    access_token=access_token,
                    refresh_token=refresh_token if isinstance(refresh_token, str) else None,
                    account_id=account_id if isinstance(account_id, str) else None,
                    storage=auth_payload.storage,
                )

        api_key = data.get("OPENAI_API_KEY")
        if isinstance(api_key, str) and api_key:
            self._auth_error = "codex_api_key"
        else:
            self._auth_error = "codex_no_chatgpt_tokens"
        return None

    @classmethod
    def _keychain_account(cls) -> str:
        codex_home = cls._codex_home().resolve(strict=False)
        digest = hashlib.sha256(str(codex_home).encode()).hexdigest()
        return f"cli|{digest[:16]}"

    @staticmethod
    def _account_id_from_id_token(id_token: object) -> str | None:
        if not isinstance(id_token, str):
            return None
        parts = id_token.split(".")
        if len(parts) < 2:
            return None
        payload = parts[1]
        padding = "=" * (-len(payload) % 4)
        try:
            import base64

            raw = base64.urlsafe_b64decode(payload + padding)
            claims = json.loads(raw)
        except (ValueError, json.JSONDecodeError):
            return None
        if not isinstance(claims, dict):
            return None
        auth = claims.get("https://api.openai.com/auth")
        if not isinstance(auth, dict):
            return None
        account_id = auth.get("chatgpt_account_id")
        return account_id if isinstance(account_id, str) else None

    def _write_auth_payload(self) -> bool:
        auth_payload = self._auth_payload
        if auth_payload is None:
            return False
        data = json.dumps(auth_payload.data, indent=2)
        if auth_payload.storage == "file" and auth_payload.path is not None:
            try:
                auth_payload.path.parent.mkdir(parents=True, exist_ok=True)
                auth_payload.path.write_text(data)
                auth_payload.path.chmod(0o600)
            except OSError as e:
                self._log(f"Codex auth.json write failed: {e}")
                return False
            return True
        if auth_payload.storage == "keychain" and auth_payload.keychain_account:
            try:
                subprocess.run(
                    [
                        "security",
                        "add-generic-password",
                        "-U",
                        "-s",
                        KEYCHAIN_SERVICE,
                        "-a",
                        auth_payload.keychain_account,
                        "-w",
                        data,
                    ],
                    check=True,
                    capture_output=True,
                    text=True,
                    timeout=10,
                )
            except subprocess.CalledProcessError as e:
                self._log(f"Codex keychain write failed (rc={e.returncode})")
                return False
            except (FileNotFoundError, subprocess.TimeoutExpired) as e:
                self._log(f"Codex keychain access failed: {e}")
                return False
            return True
        return False

    async def _poll_usage(
        self, credentials: Credentials, retry_on_401: bool = True
    ) -> Usage | None:
        headers = {
            "Accept": "application/json",
            "Authorization": f"Bearer {credentials.access_token}",
            "User-Agent": "codex-cli",
        }
        if credentials.account_id:
            headers["ChatGPT-Account-Id"] = credentials.account_id

        last_status = None
        last_text = ""
        async with httpx.AsyncClient(timeout=20.0) as http:
            for url in USAGE_URLS:
                try:
                    resp = await http.get(url, headers=headers)
                except httpx.HTTPError as e:
                    self._log(f"Codex usage call failed: {e}")
                    return None
                if resp.status_code == 404:
                    last_status = resp.status_code
                    last_text = resp.text[:200]
                    continue
                if resp.status_code == 401 and retry_on_401:
                    self._log("Codex usage HTTP 401; attempting OAuth refresh")
                    refreshed = await self._refresh_credentials(credentials)
                    if refreshed is None:
                        return Usage(0, 0, 0, 0, "codex_refresh_failed", False)
                    return await self._poll_usage(refreshed, retry_on_401=False)
                if resp.status_code >= 400:
                    self._log(f"Codex usage HTTP {resp.status_code}: {resp.text[:200]}")
                    return Usage(0, 0, 0, 0, f"codex_http_{resp.status_code}", False)
                try:
                    payload = resp.json()
                except json.JSONDecodeError:
                    self._log("Codex usage response was not JSON")
                    return Usage(0, 0, 0, 0, "codex_bad_json", False)
                return self._usage_from_payload(payload)

        self._log(f"Codex usage endpoint unavailable: HTTP {last_status} {last_text}")
        return Usage(0, 0, 0, 0, "codex_no_usage", False)

    async def _refresh_credentials(self, credentials: Credentials) -> Credentials | None:
        if not credentials.refresh_token:
            self._log("Codex refresh token unavailable")
            return None
        body = {
            "client_id": REFRESH_CLIENT_ID,
            "grant_type": "refresh_token",
            "refresh_token": credentials.refresh_token,
        }
        try:
            async with httpx.AsyncClient(timeout=30.0) as http:
                resp = await http.post(
                    REFRESH_TOKEN_URL,
                    headers={"Content-Type": "application/json"},
                    json=body,
                )
        except httpx.HTTPError as e:
            self._log(f"Codex OAuth refresh failed: {e}")
            return None
        if resp.status_code >= 400:
            self._log(f"Codex OAuth refresh HTTP {resp.status_code}")
            return None
        try:
            refresh_data = resp.json()
        except json.JSONDecodeError:
            self._log("Codex OAuth refresh response was not JSON")
            return None

        access_token = refresh_data.get("access_token")
        if not isinstance(access_token, str):
            self._log("Codex OAuth refresh response missing access token")
            return None
        refresh_token = refresh_data.get("refresh_token")
        if not isinstance(refresh_token, str):
            refresh_token = credentials.refresh_token

        auth_payload = self._auth_payload
        if auth_payload is not None:
            tokens = auth_payload.data.setdefault("tokens", {})
            if isinstance(tokens, dict):
                tokens["access_token"] = access_token
                tokens["refresh_token"] = refresh_token
                id_token = refresh_data.get("id_token")
                if isinstance(id_token, str):
                    tokens["id_token"] = id_token
                auth_payload.data["last_refresh"] = (
                    datetime.now(UTC).isoformat().replace("+00:00", "Z")
                )
                if not self._write_auth_payload():
                    return None

        self._log("Codex OAuth refresh succeeded")
        return Credentials(
            access_token=access_token,
            refresh_token=refresh_token,
            account_id=credentials.account_id,
            storage=credentials.storage,
        )

    def _usage_from_payload(self, payload: object) -> Usage:
        if not isinstance(payload, dict):
            return Usage(0, 0, 0, 0, "codex_bad_usage", False)
        rate_limit = payload.get("rate_limit")
        if not isinstance(rate_limit, dict):
            return Usage(0, 0, 0, 0, "codex_no_usage", False)

        primary = rate_limit.get("primary_window")
        secondary = rate_limit.get("secondary_window")
        if not isinstance(primary, dict) or not isinstance(secondary, dict):
            return Usage(0, 0, 0, 0, "codex_no_windows", False)

        allowed = rate_limit.get("allowed")
        limit_reached = rate_limit.get("limit_reached")
        reached_type = payload.get("rate_limit_reached_type")
        if allowed is True:
            status = "allowed"
        elif isinstance(reached_type, str):
            status = reached_type
        elif limit_reached is True:
            status = "limited"
        else:
            status = "unknown"

        return Usage(
            session_pct=self._pct(primary.get("used_percent")),
            session_reset_min=self._reset_minutes(primary),
            weekly_pct=self._pct(secondary.get("used_percent")),
            weekly_reset_min=self._reset_minutes(secondary),
            status=status,
            ok=True,
        )

    @staticmethod
    def _pct(value: object) -> int:
        try:
            pct = round(float(value))
        except (TypeError, ValueError):
            return 0
        return max(0, min(100, int(pct)))

    @staticmethod
    def _reset_minutes(window: dict[str, Any]) -> int:
        reset_at = window.get("reset_at")
        if reset_at is not None:
            try:
                minutes = (float(reset_at) - time.time()) / 60.0
                return max(0, int(round(minutes)))
            except (TypeError, ValueError):
                pass
        reset_after = window.get("reset_after_seconds")
        try:
            minutes = float(reset_after) / 60.0
        except (TypeError, ValueError):
            return 0
        return max(0, int(round(minutes)))
