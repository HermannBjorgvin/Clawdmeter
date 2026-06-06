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

# OAuth refresh — endpoint and client id lifted from the Claude Code CLI so we
# renew exactly the way it does. The access token lives ~8h; we refresh ONLY
# reactively (on a 401/403), free-riding on Claude Code's own refreshes rather
# than refreshing proactively. Proactive refresh competed with the app for the
# same endpoint and tripped its rate limit (429); while you use Claude the app
# keeps this token fresh and the daemon never has to touch the refresh endpoint.
OAUTH_TOKEN_URL = "https://platform.claude.com/v1/oauth/token"
OAUTH_CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e"
REFRESH_SKEW = 120  # retained for the expiry helper / tests; loop no longer refreshes proactively
# Hard floor between refresh ATTEMPTS (success or failure). A data-hungry device
# fires a refresh request every few seconds; without this, an expired token whose
# refresh is failing (e.g. a 429) drives the OAuth endpoint into a self-sustaining
# rate-limit loop. The base floor grows exponentially on consecutive failures
# (REFRESH_COOLDOWN → 2× → … → REFRESH_BACKOFF_CAP) so a sustained rate-limit is
# backed off rather than poked every cycle; a success resets it to the base.
REFRESH_COOLDOWN = 300       # base floor between attempts
REFRESH_BACKOFF_CAP = 1800   # backoff cap (30 min) under sustained failure
REFRESH_GIVEUP_AFTER = 5     # consecutive failures before logging a "re-login needed" notice
_last_refresh_attempt = 0.0
_refresh_failures = 0        # consecutive refresh failures (drives backoff; reset on success)


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


def _read_credentials_blob() -> tuple[Path, str] | None:
    """Return (path, raw_blob) for the first readable credential file, or None.

    The path is returned alongside the contents so a refresh write-back lands
    on the SAME file Claude Code read from. Candidate order is first-hit-wins
    (see _windows_credential_candidates), so reading and writing must agree on
    which file won — never read fallback #2 and write primary.
    """
    for path in _windows_credential_candidates():
        try:
            return path, path.read_text(encoding="utf-8")
        except OSError:
            continue
    return None


def _write_credentials_blob(path: Path, blob: str) -> bool:
    """Persist an updated credentials blob back to `path` (where it was read).

    Called only with a blob built from a *successful* token refresh, so the
    store Claude Code reads stays in sync (the provider may rotate the refresh
    token). A write failure is non-fatal — the refreshed access token still
    works in-memory for this process — so we log and carry on.

    We write plaintext JSON, exactly as the read side (read_token / _read_expiry)
    consumes it: Claude Code on Windows keeps .credentials.json as a plaintext
    file (no DPAPI blob), so round-tripping it as plaintext is what keeps the
    file readable by both Claude Code and us. Overwriting in place via
    write_text() preserves the file's existing ACLs (it truncates rather than
    recreating), so no extra permission hardening is needed here.
    """
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(blob, encoding="utf-8")
        return True
    except OSError as e:
        log(f"Error writing credentials: {e}")
        return False


def _credentials_expiry_seconds(raw: str | None = None) -> float | None:
    """Epoch-seconds at which the stored access token expires, or None.

    Claude Code stores ``expiresAt`` as epoch milliseconds; normalize to
    seconds so callers can compare against ``time.time()``. When ``raw`` is
    None, read the live credential store. (``_read_expiry`` returns a
    human-readable string for the tray; this returns a number for the
    proactive-refresh comparison.)
    """
    if raw is None:
        found = _read_credentials_blob()
        raw = found[1] if found else None
    if not raw:
        return None
    try:
        data = json.loads(raw)
    except json.JSONDecodeError:
        return None
    if not isinstance(data, dict):
        return None
    oauth = data.get("claudeAiOauth", data)
    if not isinstance(oauth, dict):
        oauth = data
    exp = oauth.get("expiresAt", oauth.get("expires_at"))
    try:
        exp = float(exp)
    except (TypeError, ValueError):
        return None
    return exp / 1000.0 if exp > 1e12 else exp


def _apply_refresh_response(oauth: dict, tok: dict) -> str | None:
    """Map an OAuth token response onto the stored credential dict in place.

    Translates snake_case response fields (access_token / refresh_token /
    expires_in) onto Claude Code's camelCase storage keys (accessToken /
    refreshToken / expiresAt-in-ms). The refresh token is only overwritten if
    the response rotates it. Returns the new access token, or None if absent.
    """
    new_access = tok.get("access_token")
    if not new_access:
        return None
    oauth["accessToken"] = new_access
    if tok.get("refresh_token"):
        oauth["refreshToken"] = tok["refresh_token"]
    if tok.get("expires_in"):
        oauth["expiresAt"] = int((time.time() + float(tok["expires_in"])) * 1000)
    return new_access


def _note_refresh_failure(reason: str) -> None:
    """Count a failed refresh so the backoff grows, and surface a one-time
    'needs re-login' notice once failures pile up. (The tray's error state and
    the device's amber status dot are the user-facing signals; this explains why.)"""
    global _refresh_failures
    _refresh_failures += 1
    if _refresh_failures == REFRESH_GIVEUP_AFTER:
        log(f"Refresh: {_refresh_failures} consecutive failures ({reason}) — the "
            f"refresh token is rate-limited or stale; run `claude login` to restore. "
            f"Backing off to {REFRESH_BACKOFF_CAP // 60}-minute retries until then.")


def _note_poll_success() -> None:
    """A poll just succeeded, so the access token is valid — clear any refresh-failure
    streak / backoff. Under free-ride the daemon recovers via Claude Code's own refresh
    (or a re-login), not its own refresh, so without this the counter would never reset
    and the backoff would stay pinned at the max retry interval (which then blocks the
    very next reactive refresh that might have succeeded)."""
    global _refresh_failures, _last_refresh_attempt
    if _refresh_failures:
        log(f"Poll OK — clearing {_refresh_failures}-failure refresh backoff")
        _refresh_failures = 0
        _last_refresh_attempt = 0.0


async def refresh_access_token() -> str | None:
    """Exchange the stored refresh token for a fresh access token.

    Mirrors Claude Code's own OAuth refresh (client id + endpoint extracted
    from the CLI). On success the renewed tokens are written back to the same
    credential file Claude Code reads. On ANY failure nothing is written, so
    existing credentials are left untouched.

    Gated by an exponential backoff: one attempt per window, the window doubling
    on each consecutive failure (REFRESH_COOLDOWN → REFRESH_BACKOFF_CAP) and
    resetting on success — so a sustained rate-limit is backed off, not hammered.
    """
    global _last_refresh_attempt, _refresh_failures
    now = time.time()
    cooldown = min(REFRESH_COOLDOWN * (2 ** _refresh_failures), REFRESH_BACKOFF_CAP)
    waited = now - _last_refresh_attempt
    if waited < cooldown:
        log(f"Refresh: skipping — backoff ({int(cooldown - waited)}s left, "
            f"{_refresh_failures} consecutive failures)")
        return None
    _last_refresh_attempt = now

    found = _read_credentials_blob()
    if not found:
        log("Refresh: no credentials available")
        return None
    path, raw = found
    try:
        data = json.loads(raw)
    except json.JSONDecodeError:
        log("Refresh: credentials are not JSON; cannot refresh")
        return None

    # Locate the dict that actually holds the tokens (top-level or nested
    # under "claudeAiOauth") so we update in place and preserve every other
    # field on write-back.
    oauth = data
    if isinstance(data, dict) and "accessToken" not in data:
        for v in data.values():
            if isinstance(v, dict) and "accessToken" in v:
                oauth = v
                break
    if not isinstance(oauth, dict):
        log("Refresh: credentials shape unexpected; cannot refresh")
        return None
    refresh = oauth.get("refreshToken") or oauth.get("refresh_token")
    if not refresh:
        log("Refresh: no refresh token present — re-login required")
        return None

    body = {
        "grant_type": "refresh_token",
        "refresh_token": refresh,
        "client_id": OAUTH_CLIENT_ID,
    }
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.post(
                OAUTH_TOKEN_URL,
                json=body,
                headers={
                    "Content-Type": "application/json",
                    "User-Agent": API_HEADERS_TEMPLATE["User-Agent"],
                },
            )
    except httpx.HTTPError as e:
        log(f"Refresh request failed: {e}")
        _note_refresh_failure("request error")
        return None
    if resp.status_code != 200:
        log(f"Refresh rejected HTTP {resp.status_code}: {resp.text[:200]}")
        _note_refresh_failure(f"HTTP {resp.status_code}")
        return None
    try:
        tok = resp.json()
    except ValueError:
        log("Refresh: response was not JSON")
        _note_refresh_failure("bad response")
        return None
    new_access = _apply_refresh_response(oauth, tok)
    if not new_access:
        log("Refresh: response contained no access_token")
        _note_refresh_failure("bad response")
        return None

    _refresh_failures = 0   # success — clear the backoff
    if _write_credentials_blob(path, json.dumps(data)):
        log("Refresh: access token renewed and written back to credential store")
    else:
        log("Refresh: access token renewed (in-memory only; write-back failed)")
    return new_access


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
            oauth = data.get("claudeAiOauth", {})
            expires_ms = oauth.get("expiresAt")
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
                # Free-ride on Claude Code's credential: use whatever access token it
                # currently holds and refresh only reactively (on a 401), never
                # proactively. Proactive refresh competed with the app's own refreshes
                # and tripped the token endpoint's rate limit (429). While you use
                # Claude / Claude Code, the app keeps this token fresh and the daemon
                # never has to touch the refresh endpoint.
                token = read_token()  # D-09: fresh each cycle
                if not token:
                    log("No token; signalling no-data to device")
                    await session.write_payload({"ok": False})
                    last_poll = time.time()
                    if tray_state:
                        tray_state.set_error("token expired — run claude login")
                else:
                    payload = None
                    auth_failed = False
                    try:
                        payload = await poll_api(token)
                    except AuthError:
                        # Token rejected despite our proactive check (e.g. it was
                        # revoked server-side). Refresh once and retry this poll
                        # before surfacing the actionable "run claude login" toast.
                        new = await refresh_access_token()
                        if new:
                            try:
                                payload = await poll_api(new)
                            except AuthError:
                                log("Still rejected after refresh; re-login may be required")
                                auth_failed = True
                                if tray_state:
                                    tray_state.set_error("token expired — run claude login")
                        else:
                            auth_failed = True
                            if tray_state:
                                tray_state.set_error("token expired — run claude login")
                    if payload is not None:
                        _note_poll_success()   # token works → clear any stale refresh backoff
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
                    elif auth_failed:
                        # Genuine auth failure (token dead, refresh can't recover): send a
                        # {"ok": false} "no-data" beat so the device shows its idle screen now
                        # instead of holding stale numbers for the full freshness window.
                        # Transient misses fall through silently below (no flicker).
                        log("No data (auth); signalling idle to device")
                        await session.write_payload({"ok": False})
                        last_poll = time.time()
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
