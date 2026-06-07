#!/usr/bin/env python3
"""Claude Usage Tracker Daemon — Windows (Phase 2).

Reads the Claude OAuth token from the native-Windows credentials path and
polls the Anthropic API for rate-limit utilization data. BLE glue added in
later plans.
"""

import asyncio
import datetime
import json
import logging
import logging.handlers
import os
import re
import signal
import sys
import tempfile
import threading
import time
from pathlib import Path

import httpx
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

DEVICE_NAME = "Claude Controller"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"

POLL_INTERVAL = 60
TICK = 5
SCAN_TIMEOUT = 8.0
CONNECT_RETRIES = 3        # D-01: attempts before giving up on a device
CONNECT_RETRY_DELAY = 2.0  # D-01: seconds between failed connect attempts
ZOMBIE_BREAK_LIMIT = 1     # D-03: consecutive write failures before abandoning a half-open link
                           # N=1: breaks at T=60s, leaves ~60s headroom for reconnect+poll inside 120s SLA
                           # N=2 would bust the 120s budget before reconnect even begins
RECONNECT_BACKOFF_CAP = 8  # D-05: fast-reconnect cap (seconds); keeps stacked retries inside 120s SLA
                           # ~5–10s band per CONTEXT.md Claude's Discretion; 8 chosen as middle ground

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

# OAuth token refresh — VERIFIED values from RESEARCH.md
# Primary: platform.claude.com (current preferred host); fallback: console.anthropic.com (proven working)
OAUTH_TOKEN_URL = "https://platform.claude.com/v1/oauth/token"
OAUTH_FALLBACK_URL = "https://console.anthropic.com/v1/oauth/token"
OAUTH_CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e"

# Proactive refresh window: refresh if token expires within this many seconds
OAUTH_PROACTIVE_REFRESH_SECS = 5 * 60  # 5 minutes


def _build_file_logger() -> logging.Logger | None:
    """Create a rotating file logger for field diagnostics, or None.

    Autostart launches the tray under pythonw.exe, which has no console — stdout
    is discarded (and is in fact None, making print() unsafe). A rotating file is
    then the ONLY trail when the daemon stalls in the field. Windows-only: on the
    Linux dev box / CI the console print() suffices, and gating to win32 keeps the
    pure-helper unit tests from writing stray log files.
    """
    if sys.platform != "win32":
        return None
    logger = logging.getLogger("clawdmeter.daemon")
    if logger.handlers:
        return logger  # idempotent across re-import (tray imports this module)
    base = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
    path = base / "Clawdmeter" / "daemon.log"
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        handler = logging.handlers.RotatingFileHandler(
            path, maxBytes=512 * 1024, backupCount=3, encoding="utf-8"
        )
    except OSError:
        return None  # best-effort — logging setup must never stop the daemon
    handler.setFormatter(logging.Formatter("%(asctime)s %(message)s", "%Y-%m-%d %H:%M:%S"))
    logger.addHandler(handler)
    logger.setLevel(logging.INFO)
    logger.propagate = False
    return logger


_FILE_LOGGER = _build_file_logger()


def log(msg: str) -> None:
    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
    # Under pythonw sys.stdout is None and print() would raise — guard it so a
    # missing console can never crash the daemon thread (the silent-freeze mode).
    try:
        print(line, flush=True)
    except (OSError, ValueError, AttributeError, RuntimeError):
        pass
    if _FILE_LOGGER is not None:
        _FILE_LOGGER.info(msg)


class AuthError(Exception):
    """Raised by poll_api on a genuine 401/403 — the token really is expired or
    invalid and the user must re-run `claude login`. Distinct from a None return,
    which means a TRANSIENT failure (network/DNS, timeout, rate-limit, 5xx) that
    must NOT be mislabeled as a token problem (SC#5: a boot-time `getaddrinfo
    failed` DNS blip wrongly fired the 'token expired' toast)."""


async def poll_api(token: str) -> dict | None:
    headers = dict(API_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {token}"
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.post(API_URL, headers=headers, json=API_BODY)
    except httpx.HTTPError as e:
        # Network/DNS/timeout — transient. Return None (no toast), retry next tick.
        log(f"API call failed: {e}")
        return None
    if resp.status_code in (401, 403):
        # Genuine auth rejection — the ONLY case that warrants the actionable
        # "run claude login" toast.
        log(f"API HTTP {resp.status_code}: {resp.text[:200]}")
        raise AuthError(resp.status_code)
    if resp.status_code >= 400:
        # Other 4xx/5xx (rate-limit, server error) — transient, not a token issue.
        log(f"API HTTP {resp.status_code}: {resp.text[:200]}")
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

    payload = {
        "s": pct(hdr("anthropic-ratelimit-unified-5h-utilization")),
        "sr": reset_minutes(hdr("anthropic-ratelimit-unified-5h-reset")),
        "w": pct(hdr("anthropic-ratelimit-unified-7d-utilization")),
        "wr": reset_minutes(hdr("anthropic-ratelimit-unified-7d-reset")),
        "st": hdr("anthropic-ratelimit-unified-5h-status", "unknown"),
        "ok": True,
    }
    return payload


async def scan_for_device():
    """Scan for DEVICE_NAME and return the BLEDevice, or None."""
    log(f"Scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT}s)...")
    device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=SCAN_TIMEOUT)
    if device:
        log(f"Found: {device.address}")
    return device  # BLEDevice or None — NOT an address string


class Session:
    def __init__(self, client: BleakClient) -> None:
        self.client = client
        self.refresh_requested = asyncio.Event()

    def _on_refresh(self, _char, _data: bytearray) -> None:
        log("Refresh requested by device")
        self.refresh_requested.set()

    async def setup_refresh_subscription(self) -> None:
        # The refresh subscription is optional — the 60s poll loop works without it.
        # WinRT's start_notify() CCCD write can raise a raw OSError/WinError (not
        # wrapped as BleakError) when the peer GATT server is transiently unavailable,
        # e.g. a just-power-cycled ESP32 whose server is not yet ready (G-03-01, SC#3).
        # Degrade gracefully instead of crashing the daemon so it stays single-process
        # across a power-cycle reconnect (SC#4, no restart).
        try:
            await self.client.start_notify(REQ_CHAR_UUID, self._on_refresh)
        except (BleakError, ValueError, OSError) as e:
            log(f"Refresh subscription unavailable: {e}")

    async def write_payload(self, payload: dict) -> bool:
        data = json.dumps(payload, separators=(",", ":")).encode()
        log(f"Sending: {data.decode()}")
        try:
            await self.client.write_gatt_char(RX_CHAR_UUID, data, response=False)
            return True
        except (BleakError, OSError) as e:
            # WinRT can raise a raw OSError/WinError (NOT wrapped as BleakError)
            # when the peer GATT server goes transiently unavailable mid-write —
            # the same failure class setup_refresh_subscription() guards against.
            # Returning False trips the zombie-link break -> clean reconnect,
            # rather than an uncaught exception killing the daemon thread (the
            # silent-freeze failure mode, SC#2 field report).
            log(f"Write failed: {e}")
            return False


def _extract_access_token(blob: str) -> str | None:
    """Pull the accessToken out of a credentials blob.

    Claude Code stores credentials as a JSON object; the blob may also be
    nested ({"claudeAiOauth": {"accessToken": "..."}}). Fall back to a
    regex match so unexpected shapes still work, and finally treat the
    blob as a raw token if nothing else matches.
    """
    blob = blob.strip()
    if not blob:
        return None
    try:
        data = json.loads(blob)
    except json.JSONDecodeError:
        data = None
    if isinstance(data, dict):
        # direct: {"accessToken": "..."}
        tok = data.get("accessToken")
        if isinstance(tok, str) and tok.strip():
            return tok
        # nested: {"claudeAiOauth": {"accessToken": "..."}}
        for v in data.values():
            if isinstance(v, dict):
                tok = v.get("accessToken")
                if isinstance(tok, str) and tok.strip():
                    return tok
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
    if m:
        return m.group(1)
    # Raw token (no JSON wrapper) — must look plausible (sk-ant-... etc.)
    if re.fullmatch(r"[A-Za-z0-9_\-.~+/=]{20,}", blob):
        return blob
    return None


def _windows_credential_candidates() -> list[Path]:
    """Return the ordered list of credential file paths to probe (first hit wins).

    Priority:
    1. CLAUDE_CREDENTIALS_PATH env override (D-03, project-specific)
    2. CLAUDE_CONFIG_DIR env override (official Claude override)
    3. D-02 candidate list: home/.claude, LOCALAPPDATA/Claude, APPDATA/Claude
    """
    # Priority 1: project-specific env override (D-03)
    if override := os.environ.get("CLAUDE_CREDENTIALS_PATH"):
        return [Path(override)]
    # Priority 2: official CLAUDE_CONFIG_DIR env override
    if config_dir := os.environ.get("CLAUDE_CONFIG_DIR"):
        return [Path(config_dir) / ".credentials.json"]
    # Priority 3: D-02 candidate list — first hit wins
    home = Path.home()
    local_appdata = Path(os.environ.get("LOCALAPPDATA", home / "AppData" / "Local"))
    appdata = Path(os.environ.get("APPDATA", home / "AppData" / "Roaming"))
    return [
        home / ".claude" / ".credentials.json",          # primary (confirmed by docs)
        local_appdata / "Claude" / ".credentials.json",  # fallback 2
        appdata / "Claude" / ".credentials.json",        # fallback 3
    ]


def read_token() -> str | None:
    """Read the Claude OAuth access token from the first available credential file."""
    for path in _windows_credential_candidates():
        try:
            return _extract_access_token(path.read_text(encoding="utf-8"))
        except OSError:
            continue
    return None


def _parse_expiry_ms(full_obj: dict) -> int | None:
    """Return claudeAiOauth.expiresAt as int epoch milliseconds, or None.

    Accepts the full credential object ({"claudeAiOauth": {...}}).
    Returns None if absent, non-numeric, or the claudeAiOauth key is missing.
    Factored out of _read_expiry so callers needing the raw ms value (proactive
    refresh check, write-back) share a single parser.
    """
    try:
        oauth = full_obj.get("claudeAiOauth", {})
        val = oauth.get("expiresAt")
        if val is None:
            return None
        return int(val)
    except (TypeError, ValueError, AttributeError):
        return None


def _read_expiry() -> str:
    """Return human-readable expiry from the first-hit credentials file.

    Reads claudeAiOauth.expiresAt (epoch milliseconds — JS convention).
    Divides by 1000 before passing to fromtimestamp (Python expects seconds).
    Returns 'expiry unknown' on any parse failure.
    """
    for path in _windows_credential_candidates():
        try:
            raw = path.read_text(encoding="utf-8")
        except OSError:
            continue
        try:
            data = json.loads(raw)
            expires_ms = _parse_expiry_ms(data)
            if expires_ms is None:
                return "expiry unknown"
            # CRITICAL: expiresAt is JS-convention epoch milliseconds; divide by 1000
            # before fromtimestamp (Python expects seconds). Raw value -> year ~57000.
            dt = datetime.datetime.fromtimestamp(
                expires_ms / 1000, tz=datetime.timezone.utc
            )
            return dt.strftime("%Y-%m-%d %H:%M UTC")
        except (TypeError, ValueError, OSError, AttributeError, json.JSONDecodeError):
            return "expiry unknown"
    return "expiry unknown"


def _read_full_credentials_with_path() -> tuple[dict | None, Path | None]:
    """Like _read_full_credentials but also returns the path actually read.

    Threading the resolved path out lets a refresh write back to the SAME file
    it read, closing the TOCTOU window where a second candidate scan could pick
    a different path if files shift between read and write (WR-03).

    Returns (full_obj, path) on success, (None, None) if no file is found, and
    (None, path) if the found file is not parseable JSON (so callers can still
    report which file was bad).
    """
    for path in _windows_credential_candidates():
        try:
            raw = path.read_text(encoding="utf-8")
        except OSError:
            continue
        try:
            return json.loads(raw), path
        except (json.JSONDecodeError, ValueError):
            return None, path
    return None, None


def _read_full_credentials() -> dict | None:
    """Return the full parsed credential object from the first-hit candidate path.

    Reuses _windows_credential_candidates() for path resolution. Returns the
    WHOLE top-level dict so callers can mutate claudeAiOauth and re-dump all
    keys (subscriptionType, rateLimitTier, etc.) untouched.
    Returns None if no file is found or the file is not parseable JSON.
    """
    obj, _ = _read_full_credentials_with_path()
    return obj


def _atomic_write_credentials(path: Path, full_obj: dict) -> None:
    """Write full_obj to path atomically using same-directory tempfile + os.replace.

    Recipe:
    - mkstemp in the SAME directory (required for os.replace to be a same-volume rename)
    - json.dump with indent=2 (matches Claude Code formatting)
    - f.flush() + os.fsync() before close (durability)
    - os.replace(tmp, path) — atomic on both POSIX and Windows; overwrites existing target
    - On any exception: unlink the temp file and re-raise (no half-written creds)

    Never logs token values — only logs a success message with the path.
    """
    d = path.parent
    fd, tmp = tempfile.mkstemp(dir=d, prefix=".cred-", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(full_obj, f, indent=2, ensure_ascii=False)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
        log(f"Credentials written to {path.name} (atomic replace)")
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


async def _refresh_oauth_token(refresh_token: str) -> dict | None:
    """POST the refreshToken to the OAuth endpoint and return the parsed response dict.

    Tries OAUTH_TOKEN_URL (platform.claude.com) first. On a connection error or a
    non-OAuth 404, falls back to OAUTH_FALLBACK_URL (console.anthropic.com).

    Returns:
      dict  — parsed 200 response body (access_token, refresh_token?, expires_in, scope?)
      None  — transient/network failure (5xx, timeout, unexpected status) — caller treats as None (no toast)

    Raises:
      AuthError — genuine auth failure (400/401/403 from the token endpoint)

    Never logs raw token values — logs only lengths or human-readable expiry.
    """
    body = {
        "grant_type": "refresh_token",
        "refresh_token": refresh_token,
        "client_id": OAUTH_CLIENT_ID,
    }
    urls_to_try = [OAUTH_TOKEN_URL, OAUTH_FALLBACK_URL]
    for i, url in enumerate(urls_to_try):
        try:
            async with httpx.AsyncClient(timeout=20.0) as http:
                resp = await http.post(url, json=body)
        except httpx.HTTPError as e:
            if i < len(urls_to_try) - 1:
                log(f"OAuth refresh: connection error on {url}, trying fallback: {e}")
                continue
            log(f"OAuth refresh: transient network error: {type(e).__name__}")
            return None

        if resp.status_code == 200:
            return resp.json()

        if resp.status_code in (400, 401, 403):
            # Genuine auth failure — refresh token is revoked/expired/invalid.
            # Gate purely on status code; parse body only for redacted logging.
            try:
                err_body = resp.json()
                err_code = err_body.get("error", "unknown")
            except Exception:
                err_code = "unknown"
            log(f"OAuth refresh: genuine auth failure {resp.status_code} ({err_code})")
            raise AuthError(f"OAuth refresh failed: {resp.status_code} {err_code}")

        # 404 on primary might mean wrong host — try fallback once
        if resp.status_code == 404 and i < len(urls_to_try) - 1:
            log(f"OAuth refresh: 404 on {url}, trying fallback")
            continue

        # 5xx or other — transient
        log(f"OAuth refresh: transient HTTP {resp.status_code}")
        return None

    # Exhausted all URLs without success (should not normally reach here)
    return None


async def get_valid_token(tray_state=None) -> str | None:
    """Return a valid access token, refreshing proactively if near expiry.

    Flow:
    1. Read full credentials from disk.
    2. If accessToken present AND expiresAt > now + ~5 min: return immediately (no network).
    3. RE-READ disk (race rule: Claude Code may have refreshed concurrently). Re-check expiry.
       Still stale -> call _refresh_oauth_token with the on-disk refreshToken.
    4. On 200: build new claudeAiOauth, _atomic_write_credentials the FULL object, return new token.
    5. On AuthError from refresh: propagate (genuine — caller toasts "run claude login").
    6. On transient (None from refresh): return None (no toast, next tick retries).

    Never logs raw token values.
    """
    now_ms = int(time.time() * 1000)
    threshold_ms = now_ms + OAUTH_PROACTIVE_REFRESH_SECS * 1000

    # Step 1: read full credentials
    full_obj = _read_full_credentials()
    if full_obj is None:
        log("get_valid_token: no credentials found")
        return None

    oauth = full_obj.get("claudeAiOauth", {})
    access_token = oauth.get("accessToken", "")
    expires_ms = _parse_expiry_ms(full_obj)

    # Step 2: token is present and not near expiry — return immediately
    if access_token and expires_ms is not None and expires_ms > threshold_ms:
        return access_token

    # Step 3: RE-READ disk before spending the refresh token (race-condition mitigation:
    # Claude Code may have just written a fresh token; single-use refresh tokens mean we
    # must not use a cached possibly-already-rotated refresh token). Capture the resolved
    # path so the write-back below targets the exact file we read (WR-03 — no second scan).
    full_obj, creds_path = _read_full_credentials_with_path()
    if full_obj is None:
        log("get_valid_token: credentials vanished on re-read")
        return None

    oauth = full_obj.get("claudeAiOauth", {})
    access_token = oauth.get("accessToken", "")
    expires_ms = _parse_expiry_ms(full_obj)

    if access_token and expires_ms is not None and expires_ms > threshold_ms:
        # Fresh token appeared on disk (Claude Code refreshed concurrently) — use it
        log("get_valid_token: on-disk token already fresh after re-read, skipping network refresh")
        return access_token

    stored_refresh_token = oauth.get("refreshToken", "")
    if not stored_refresh_token:
        log("get_valid_token: no refreshToken on disk, cannot refresh")
        return None

    log(f"get_valid_token: token near expiry or expired, refreshing (len={len(stored_refresh_token)})")

    # Step 4/5/6: call the OAuth endpoint
    refresh_result = await _refresh_oauth_token(stored_refresh_token)
    if refresh_result is None:
        # Transient failure — no toast
        log("get_valid_token: transient refresh failure, will retry next tick")
        return None

    # Successful 200 response — build updated credential object
    new_access_token = refresh_result.get("access_token", "")
    if not new_access_token:
        # 200 but no usable access_token. Treat as TRANSIENT (no toast) and do NOT
        # write back — writing an empty token would burn the single-use refresh
        # token and trip the proactive check every tick, spinning a refresh loop
        # against a misbehaving server (WR-01).
        log("get_valid_token: refresh 200 but empty access_token; treating as transient, not writing back")
        return None
    new_refresh_token = refresh_result.get("refresh_token", stored_refresh_token)  # reuse old if absent
    expires_in_secs = refresh_result.get("expires_in", 0)
    new_expires_ms = int(time.time() * 1000) + expires_in_secs * 1000

    # Mutate only the token-related keys; all other keys (subscriptionType, rateLimitTier, etc.) preserved
    oauth["accessToken"] = new_access_token
    oauth["refreshToken"] = new_refresh_token
    oauth["expiresAt"] = new_expires_ms

    # Update scopes array only if the response included the scope field
    scope_str = refresh_result.get("scope")
    if scope_str:
        oauth["scopes"] = scope_str.split()

    full_obj["claudeAiOauth"] = oauth

    # Write back to the EXACT path resolved during the race re-read above (WR-03) —
    # no second candidate scan, so a file shift between read and write can't make us
    # update the wrong file.
    if creds_path is not None:
        _atomic_write_credentials(creds_path, full_obj)
        expiry_dt = datetime.datetime.fromtimestamp(
            new_expires_ms / 1000, tz=datetime.timezone.utc
        )
        log(f"get_valid_token: refreshed, new expiry {expiry_dt.strftime('%Y-%m-%d %H:%M UTC')}")
    else:
        log("get_valid_token: refreshed but could not find creds path to write back")

    return new_access_token


async def _poll_with_refresh(token: str, tray_state=None) -> dict | None:
    """Call poll_api(token); on AuthError, attempt ONE forced refresh + ONE retry.

    Extracted from connect_and_run to keep the reactive-retry logic unit-testable
    without driving the full BLE session loop.

    Returns:
      dict  — payload from poll_api (first try or after successful forced refresh)
      None  — any failure path that should NOT toast (transient poll, transient refresh)

    Side effects (toast "token expired — run claude login"):
      ONLY when the forced refresh itself raises AuthError (genuine invalid token).

    AuthError from poll_api is NOT re-raised — it is caught here and triggers the
    forced refresh + retry.  The caller receives None or payload, never AuthError.
    """
    try:
        return await poll_api(token)
    except AuthError:
        # Unexpected poll 401/403 — one forced refresh + one retry before toasting.
        log("poll_api returned 401/403; attempting forced token refresh before toasting")

    try:
        new_token = await get_valid_token(tray_state)
    except AuthError:
        # Forced refresh also failed genuinely — NOW toast.
        log("Forced refresh also failed (genuine); firing 'run claude login' toast")
        if tray_state:
            tray_state.set_error("token expired — run claude login")
        return None

    if new_token is None:
        # Transient refresh failure — no toast, next tick retries.
        log("Forced refresh returned None (transient); leaving tray unchanged")
        return None

    # Retry poll once with the fresh token
    try:
        return await poll_api(new_token)
    except AuthError:
        # Retry also rejected — now toast.
        log("Retry poll also returned 401/403; firing 'run claude login' toast")
        if tray_state:
            tray_state.set_error("token expired — run claude login")
        return None


async def _wait_first(*events: asyncio.Event, timeout: float) -> None:
    """Return when any of `events` is set, or after `timeout` seconds.

    Lets the poll loop's TICK wait wake immediately on a stop signal (clean,
    responsive Quit) without losing the refresh-request wakeup — instead of
    waiting only on refresh_requested and re-checking stop_event up to TICK
    later. Cancels and drains the loser tasks so they don't warn.
    """
    tasks = [asyncio.ensure_future(e.wait()) for e in events]
    try:
        await asyncio.wait(tasks, timeout=timeout, return_when=asyncio.FIRST_COMPLETED)
    finally:
        for t in tasks:
            t.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)


async def connect_and_run(device, stop_event: asyncio.Event, tray_state=None) -> bool:
    """Connect to device and poll until disconnected or stopped.

    Returns True if at least one successful write occurred.
    """
    log(f"Connecting to {device.address}...")
    # D-01: retry wrapper — defeats WinRT post-wake failure modes
    # (Could not get GATT services: Unreachable, stale is_connected).
    # Rebuild a fresh BleakClient each attempt (locked D-05 recipe).
    client = None
    for attempt in range(CONNECT_RETRIES):
        # D-05: pass BLEDevice (not address string), address_type="random" (NimBLE
        # static-random), use_cached_services=False (DIY firmware — WinRT GATT cache
        # may be stale after firmware reflash).
        client = BleakClient(
            device,
            address_type="random",
            use_cached_services=False,
        )
        try:
            await client.connect()
        except (BleakError, asyncio.TimeoutError) as e:
            log(f"Connection attempt {attempt + 1}/{CONNECT_RETRIES} failed: {e}")
            try:
                await client.disconnect()
            except BleakError:
                pass
            if attempt < CONNECT_RETRIES - 1:
                await asyncio.sleep(CONNECT_RETRY_DELAY)
            continue

        if not client.is_connected:
            log(f"Connection attempt {attempt + 1}/{CONNECT_RETRIES} failed (not connected)")
            try:
                await client.disconnect()
            except BleakError:
                pass
            if attempt < CONNECT_RETRIES - 1:
                await asyncio.sleep(CONNECT_RETRY_DELAY)
            continue

        # Connected successfully
        break
    else:
        log(f"Connection failed after {CONNECT_RETRIES} attempts")
        return False

    log("Connected")
    session = Session(client)
    await session.setup_refresh_subscription()

    last_poll = 0.0  # D-03: poll immediately on first connect
    used_successfully = False
    consecutive_failures = 0  # D-03: zombie-link break counter
    try:
        while client.is_connected and not stop_event.is_set():
            now = time.time()
            elapsed = now - last_poll
            if session.refresh_requested.is_set() or elapsed >= POLL_INTERVAL:
                session.refresh_requested.clear()
                # Proactive refresh: get_valid_token() returns a fresh token (possibly
                # refreshing before the poll if near expiry), or None on transient
                # failure (which must NOT toast — same as a transient poll failure).
                # A genuine refresh rejection raises AuthError — catch it here, toast,
                # and keep the loop alive. Letting it propagate would kill the daemon
                # thread (no handler exists above), the silent-freeze failure mode (CR-01).
                try:
                    token = await get_valid_token(tray_state)  # D-09: fresh each cycle, with refresh
                except AuthError:
                    log("Proactive refresh failed genuinely; firing 'run claude login' toast")
                    if tray_state:
                        tray_state.set_error("token expired — run claude login")
                    token = None
                if not token:
                    log("No token; skipping poll")
                    # Do NOT toast for a transient get_valid_token None — a missing
                    # file or transient refresh failure should not show "run claude login".
                    # (A genuine AuthError was already handled + toasted just above.)
                else:
                    # _poll_with_refresh handles: first poll OK -> payload; poll 401 ->
                    # forced refresh + retry; retry OK -> payload; forced refresh genuine
                    # AuthError -> toast + None; transient -> None (no toast).
                    payload = await _poll_with_refresh(token, tray_state)
                    if payload is not None:
                        if await session.write_payload(payload):
                            last_poll = time.time()
                            used_successfully = True
                            consecutive_failures = 0  # D-03: reset on success
                            if tray_state:
                                tray_state.set_connected(time.time())
                        else:
                            consecutive_failures += 1
                            if consecutive_failures >= ZOMBIE_BREAK_LIMIT:
                                log(
                                    f"Zombie link detected ({consecutive_failures} consecutive"
                                    f" write failures); abandoning connection"
                                )
                                break
                    # else: payload is None from a TRANSIENT failure (network/DNS,
                    # timeout, rate-limit, 5xx). poll_api already logged it; do NOT
                    # toast "token expired" — that mislabeled a boot-time DNS blip
                    # as an auth problem (SC#5). Leave tray state unchanged; the next
                    # tick retries and set_connected() recovers it.

            # Wake on a refresh request OR a stop, whichever comes first. Waking
            # promptly on stop_event is what lets the finally below run
            # client.disconnect() before the process exits, so the peer gets a
            # clean GATT disconnect (returns to its waiting screen) instead of
            # being left frozen on stale data after Quit (SC#3 graceful shutdown).
            await _wait_first(session.refresh_requested, stop_event, timeout=TICK)
    finally:
        # Clean GATT disconnect on the way out — this is what tells the peripheral
        # the link is gone. WinRT can surface a raw OSError (not BleakError) here,
        # so swallow both; the link tears down regardless once we exit.
        try:
            await client.disconnect()
        except (BleakError, OSError):
            pass

    log("Device disconnected" if not stop_event.is_set() else "Stopping")
    return used_successfully


def _next_backoff(current: int, cap: int) -> int:
    """D-05: double current backoff value, clamped to cap.

    Pure helper — unit-testable without driving the main loop.
    Used by both slow-search (cap=60) and fast-reconnect (cap=RECONNECT_BACKOFF_CAP) regimes.
    """
    return min(current * 2, cap)


async def main(tray_state=None) -> None:
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    # Populate the shared state object so the tray can route Quit through
    # loop.call_soon_threadsafe (RESEARCH Pitfall 2).  Additive — the existing
    # stop_event = asyncio.Event() line above is unchanged.
    if tray_state is not None:
        tray_state.loop = loop
        tray_state.stop_event = stop_event

    def _stop(*_args: object) -> None:
        log("Daemon stopping")
        stop_event.set()

    # OS signal handlers can only be installed from the main thread, and
    # loop.add_signal_handler is unsupported on Windows. When running under the
    # tray (04-03) the loop lives in a background thread and the tray owns clean
    # shutdown via stop_event (loop.call_soon_threadsafe), so skip silently there.
    if threading.current_thread() is threading.main_thread():
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, _stop)
            except NotImplementedError:
                # Windows: add_signal_handler not supported; fall back to signal.signal
                try:
                    signal.signal(sig, _stop)
                except ValueError:
                    # Not the main thread of the main interpreter — tray owns shutdown.
                    pass

    log("=== Claude Usage Tracker Daemon (BLE, Windows) ===")
    log(f"Poll interval: {POLL_INTERVAL}s")

    # D-05: two distinct backoff regimes — slow-search (device absent) vs fast-reconnect (link dropped)
    search_backoff = 1     # caps at 60s — gentle, for a device that is genuinely absent/off
    reconnect_backoff = 1  # caps at RECONNECT_BACKOFF_CAP — fast, to clear the 120s SLA after a drop
    while not stop_event.is_set():
        device = await scan_for_device()
        if not device:
            # Slow-search regime: device was not found by scan — back off gently
            if tray_state:
                tray_state.set_scanning()
            log(f"Device not found, retrying in {search_backoff}s...")
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=search_backoff)
            except asyncio.TimeoutError:
                pass
            search_backoff = _next_backoff(search_backoff, 60)
            continue

        ok = await connect_and_run(device, stop_event, tray_state)
        if not ok:
            # Fast-reconnect regime: had/attempted a link that dropped — retry quickly
            if tray_state:
                tray_state.set_scanning()
            log(f"Connection lost, reconnecting in {reconnect_backoff}s...")
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=reconnect_backoff)
            except asyncio.TimeoutError:
                pass
            reconnect_backoff = _next_backoff(reconnect_backoff, RECONNECT_BACKOFF_CAP)
        else:
            # Successful session — reset reconnect counter to floor; search_backoff also reset
            reconnect_backoff = 1
            search_backoff = 1


if __name__ == "__main__":
    if sys.platform != "win32":
        print(
            "Warning: running under Linux/WSL — WinRT BLE will not be available.",
            file=sys.stderr,
        )
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
