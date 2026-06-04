#!/usr/bin/env python3
"""Claude Usage Tracker Daemon (BLE) — macOS port of claude-usage-daemon.sh.

Polls Claude API rate-limit headers and writes a JSON payload to the
ESP32 "Clawdmeter" peripheral over a custom GATT service. Uses
bleak (CoreBluetooth backend on macOS).
"""

import asyncio
import getpass
import json
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

import httpx
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

DEVICE_NAME = "Clawdmeter"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"

POLL_INTERVAL = 60
TICK = 5
SCAN_TIMEOUT = 8.0

# macOS: token lives in Keychain (service "Claude Code-credentials").
# Linux: token lives in ~/.claude/.credentials.json.
KEYCHAIN_SERVICE = "Claude Code-credentials"
CREDENTIALS_PATH = Path.home() / ".claude" / ".credentials.json"
SAVED_ADDR_FILE = Path.home() / ".config" / "claude-usage-monitor" / "ble-address"

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
# renew exactly the way it does. The access token lives ~8h; we refresh on
# demand (on a 401/403) and proactively when it is within REFRESH_SKEW seconds
# of expiry, so the device never sees a stale-token stall.
OAUTH_TOKEN_URL = "https://platform.claude.com/v1/oauth/token"
OAUTH_CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e"
REFRESH_SKEW = 120


class TokenExpired(Exception):
    """Raised by poll_api on a 401/403 so the caller can refresh and retry."""


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


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
        if isinstance(data.get("accessToken"), str):
            return data["accessToken"]
        # nested: {"claudeAiOauth": {"accessToken": "..."}}
        for v in data.values():
            if isinstance(v, dict) and isinstance(v.get("accessToken"), str):
                return v["accessToken"]
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
    if m:
        return m.group(1)
    # Raw token (no JSON wrapper) — must look plausible (sk-ant-... etc.)
    if re.fullmatch(r"[A-Za-z0-9_\-.~+/=]{20,}", blob):
        return blob
    return None


def _decode_keychain_blob(raw: str) -> str:
    """Transparently decode a hex-dumped Keychain secret back to text.

    ``security … -w`` prints the password as a continuous hex string whenever
    the stored bytes aren't cleanly printable (e.g. an embedded newline). A
    normal credentials blob is JSON, which is never valid hex (it contains
    '{', '"', …), so all-hex detection is unambiguous and safe.
    """
    s = raw.strip()
    if s and len(s) % 2 == 0 and re.fullmatch(r"[0-9a-fA-F]+", s):
        try:
            return bytes.fromhex(s).decode("utf-8")
        except (ValueError, UnicodeDecodeError):
            return raw
    return raw


def _read_credentials_raw() -> str | None:
    """Return the raw credentials blob (a JSON string) or None.

    macOS: the Keychain generic-password item Claude Code writes.
    Linux: the ~/.claude/.credentials.json file.
    """
    if sys.platform == "darwin":
        try:
            out = subprocess.run(
                [
                    "security",
                    "find-generic-password",
                    "-s",
                    KEYCHAIN_SERVICE,
                    "-a",
                    getpass.getuser(),
                    "-w",
                ],
                check=True,
                capture_output=True,
                text=True,
                timeout=10,
            )
        except subprocess.CalledProcessError as e:
            log(f"Keychain read failed (rc={e.returncode}): {e.stderr.strip()}")
            return None
        except (FileNotFoundError, subprocess.TimeoutExpired) as e:
            log(f"Keychain access error: {e}")
            return None
        return _decode_keychain_blob(out.stdout)
    try:
        return CREDENTIALS_PATH.read_text()
    except OSError as e:
        log(f"Error reading credentials: {e}")
        return None


def _write_credentials_raw(blob: str) -> bool:
    """Persist an updated credentials blob back to the OS credential store.

    Called only with a blob built from a *successful* token refresh, so the
    store Claude Code reads stays in sync (the provider may rotate the refresh
    token). A write failure is non-fatal — the refreshed access token still
    works in-memory for this process — so we log and carry on.

    Note: on macOS the blob is passed to ``security`` via argv (-w), so it is
    briefly visible in ``ps`` to this same user. That is not a meaningful leak
    here: any process of this user can already read the item out of the
    unlocked login keychain without authorization.
    """
    if sys.platform == "darwin":
        try:
            subprocess.run(
                [
                    "security",
                    "add-generic-password",
                    "-U",  # update the existing item in place
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
            return True
        except subprocess.CalledProcessError as e:
            log(f"Keychain write failed (rc={e.returncode}): {e.stderr.strip()}")
            return False
        except (FileNotFoundError, subprocess.TimeoutExpired) as e:
            log(f"Keychain write error: {e}")
            return False
    try:
        CREDENTIALS_PATH.parent.mkdir(parents=True, exist_ok=True)
        CREDENTIALS_PATH.write_text(blob)
        os.chmod(CREDENTIALS_PATH, 0o600)
        return True
    except OSError as e:
        log(f"Error writing credentials: {e}")
        return False


def read_token() -> str | None:
    raw = _read_credentials_raw()
    return _extract_access_token(raw) if raw else None


def _credentials_expiry_seconds(raw: str | None = None) -> float | None:
    """Epoch-seconds at which the stored access token expires, or None.

    Claude Code stores ``expiresAt`` as epoch milliseconds; normalize to
    seconds so callers can compare against ``time.time()``.
    """
    if raw is None:
        raw = _read_credentials_raw()
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


async def refresh_access_token() -> str | None:
    """Exchange the stored refresh token for a fresh access token.

    Mirrors Claude Code's own OAuth refresh (client id + endpoint extracted
    from the CLI). On success the renewed tokens are written back to the
    shared credential store. On ANY failure nothing is written, so existing
    credentials are left untouched.
    """
    raw = _read_credentials_raw()
    if not raw:
        log("Refresh: no credentials available")
        return None
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
        return None
    if resp.status_code != 200:
        log(f"Refresh rejected HTTP {resp.status_code}: {resp.text[:200]}")
        return None
    try:
        tok = resp.json()
    except ValueError:
        log("Refresh: response was not JSON")
        return None
    new_access = _apply_refresh_response(oauth, tok)
    if not new_access:
        log("Refresh: response contained no access_token")
        return None

    if _write_credentials_raw(json.dumps(data)):
        log("Refresh: access token renewed and written back to credential store")
    else:
        log("Refresh: access token renewed (in-memory only; write-back failed)")
    return new_access


def load_cached_address() -> str | None:
    if not SAVED_ADDR_FILE.exists():
        return None
    addr = SAVED_ADDR_FILE.read_text().strip()
    # Accept both Linux MAC (AA:BB:CC:DD:EE:FF) and macOS CoreBluetooth UUID
    # (E621E1F8-C36C-495A-93FC-0C247A3E6E5F).
    if re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", addr) or re.fullmatch(
        r"[0-9A-Fa-f]{8}-(?:[0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}", addr
    ):
        return addr
    log("Cached address malformed, discarding")
    SAVED_ADDR_FILE.unlink(missing_ok=True)
    return None


def save_address(addr: str) -> None:
    SAVED_ADDR_FILE.parent.mkdir(parents=True, exist_ok=True)
    SAVED_ADDR_FILE.write_text(addr)


async def scan_for_device() -> str | None:
    log(f"Scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT}s)...")
    devices = await BleakScanner.discover(timeout=SCAN_TIMEOUT)
    for d in devices:
        if d.name == DEVICE_NAME:
            log(f"Found: {d.address}")
            return d.address
    return None


# --- macOS: recover a device the OS already holds as an HID keyboard --------
#
# The firmware advertises as a BLE HID keyboard so its buttons type into the
# Mac. macOS auto-connects to that HID, and CoreBluetooth then EXCLUDES the
# peripheral from BleakScanner.discover() results (already-connected devices
# never appear in scans). bleak's connect-by-address path also scans
# internally, so a cached address can't help either. The documented escape
# hatch is retrieveConnectedPeripheralsWithServices_, which returns
# peripherals the system is already connected to. We wrap the result in a
# BLEDevice carrying the live (peripheral, manager) details so BleakClient
# connects to it directly without scanning. CoreBluetooth shares the single
# physical link, so this rides the existing HID connection — the keyboard
# keeps working.
_cb_manager = None  # reused CentralManagerDelegate (CoreBluetooth)


async def _get_cb_manager():
    """Lazily create and ready a shared CoreBluetooth central manager."""
    global _cb_manager
    if _cb_manager is None:
        from bleak.backends.corebluetooth.CentralManagerDelegate import (
            CentralManagerDelegate,
        )

        mgr = CentralManagerDelegate()
        await mgr.wait_until_ready()  # raises if Bluetooth is unauthorized/off
        _cb_manager = mgr
    return _cb_manager


async def retrieve_connected_macos(skip_addr: str | None = None):
    """Return a BLEDevice for a system-connected 'Claude Controller', or None.

    Two-step lookup, strongest signal first:

    1. Peripherals connected under our CUSTOM service UUID. Membership in
       that service is unambiguous (no other device exposes it), so we accept
       by service alone — the peripheral's name can be None on macOS.
    2. Fall back to the generic HID service 0x1812, but ONLY trust a
       peripheral whose name matches DEVICE_NAME. 0x1812 also matches
       unrelated keyboards/mice, so picking blindly here could grab the
       wrong device.

    ``skip_addr`` skips a peripheral whose UUID just failed to connect, so a
    stale CoreBluetooth handle can't trap us into never trying a fresh scan.
    """
    from CoreBluetooth import CBUUID
    from bleak.backends.device import BLEDevice

    try:
        manager = await _get_cb_manager()
    except Exception as e:  # BleakBluetoothNotAvailableError etc.
        log(f"CoreBluetooth unavailable: {e}")
        return None

    cm = manager.central_manager

    def _wrap(p):
        addr = p.identifier().UUIDString()
        log(f"Found system-connected peripheral: {p.name()!r} [{addr}]")
        return BLEDevice(addr, p.name(), (p, manager))

    def _ok(p) -> bool:
        return not (skip_addr and p.identifier().UUIDString() == skip_addr)

    # 1. Custom service — accept by service membership alone.
    custom = cm.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_(SERVICE_UUID)]
    )
    for p in custom or []:
        if _ok(p):
            return _wrap(p)

    # 2. Generic HID service — require an exact name match.
    hid = cm.retrieveConnectedPeripheralsWithServices_(
        [CBUUID.UUIDWithString_("1812")]
    )
    for p in hid or []:
        if _ok(p) and p.name() == DEVICE_NAME:
            return _wrap(p)

    return None


async def discover_target(skip_addr: str | None = None):
    """Return a connectable target, or None.

    macOS: prefer the system-connected peripheral (HID-grabbed devices are
    invisible to scans); fall back to a normal scan that yields a BLEDevice
    so the subsequent connect doesn't have to re-scan. ``skip_addr`` is
    forwarded so a just-failed peripheral is skipped, making the scan
    fallback reachable.

    Other platforms: keep the original cached-address / scan-by-name flow.
    A freshly scanned address is cached here (the only place it's saved).
    """
    if sys.platform == "darwin":
        dev = await retrieve_connected_macos(skip_addr=skip_addr)
        if dev is not None:
            return dev
        log(f"Not held by OS; scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT}s)...")
        dev = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=SCAN_TIMEOUT)
        if dev:
            log(f"Found: {dev.address}")
        return dev

    address = load_cached_address()
    if not address:
        address = await scan_for_device()
        if address:
            save_address(address)  # cache only freshly-scanned addresses
    return address


async def poll_api(token: str) -> dict | None:
    headers = dict(API_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {token}"
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.post(API_URL, headers=headers, json=API_BODY)
    except httpx.HTTPError as e:
        log(f"API call failed: {e}")
        return None
    if resp.status_code in (401, 403):
        log(f"API HTTP {resp.status_code} (token expired/invalid)")
        raise TokenExpired()
    if resp.status_code >= 400:
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


class Session:
    def __init__(self, client: BleakClient) -> None:
        self.client = client
        self.refresh_requested = asyncio.Event()

    def _on_refresh(self, _char, _data: bytearray) -> None:
        log("Refresh requested by device")
        self.refresh_requested.set()

    async def setup_refresh_subscription(self) -> None:
        try:
            await self.client.start_notify(REQ_CHAR_UUID, self._on_refresh)
        except (BleakError, ValueError) as e:
            log(f"Refresh subscription unavailable: {e}")

    async def write_payload(self, payload: dict) -> bool:
        data = json.dumps(payload, separators=(",", ":")).encode()
        log(f"Sending: {data.decode()}")
        try:
            await self.client.write_gatt_char(RX_CHAR_UUID, data, response=False)
            return True
        except BleakError as e:
            log(f"Write failed: {e}")
            return False


async def connect_and_run(target, stop_event: asyncio.Event) -> bool:
    """Connect to a target and poll until disconnected or stopped.

    ``target`` is either an address string (Linux) or a BLEDevice carrying
    live CoreBluetooth details (macOS). Returns True if the connection was
    used successfully (so the caller keeps the cached address), False if the
    connection failed and the cache should be invalidated.
    """
    display = target if isinstance(target, str) else target.address
    log(f"Connecting to {display}...")
    client = BleakClient(target)
    try:
        await client.connect()
    except (BleakError, asyncio.TimeoutError) as e:
        log(f"Connection failed: {e}")
        return False

    if not client.is_connected:
        log("Connection failed (no error but not connected)")
        return False

    log("Connected")
    session = Session(client)
    await session.setup_refresh_subscription()

    last_poll = 0.0
    used_successfully = False
    try:
        while client.is_connected and not stop_event.is_set():
            now = time.time()
            elapsed = now - last_poll
            if session.refresh_requested.is_set() or elapsed >= POLL_INTERVAL:
                session.refresh_requested.clear()
                token = read_token()
                # Proactively renew before the API would reject us, so a poll
                # never has to fail-then-retry in the steady state.
                exp = _credentials_expiry_seconds()
                if token and exp is not None and exp - time.time() < REFRESH_SKEW:
                    log("Access token near expiry; refreshing proactively")
                    token = await refresh_access_token() or token
                if not token:
                    log("No token; skipping poll")
                else:
                    payload = None
                    try:
                        payload = await poll_api(token)
                    except TokenExpired:
                        # Token rejected despite our proactive check (e.g. it
                        # was revoked). Refresh once and retry this poll.
                        new = await refresh_access_token()
                        if new:
                            try:
                                payload = await poll_api(new)
                            except TokenExpired:
                                log("Still rejected after refresh; re-login may be required")
                    if payload is not None:
                        if await session.write_payload(payload):
                            last_poll = time.time()
                            used_successfully = True

            try:
                await asyncio.wait_for(session.refresh_requested.wait(), timeout=TICK)
            except asyncio.TimeoutError:
                pass
    finally:
        try:
            await client.disconnect()
        except BleakError:
            pass

    log("Device disconnected" if not stop_event.is_set() else "Stopping")
    return used_successfully


async def main() -> None:
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    def _stop(*_args: object) -> None:
        log("Daemon stopping")
        stop_event.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _stop)
        except NotImplementedError:
            signal.signal(sig, _stop)

    log("=== Claude Usage Tracker Daemon (BLE, macOS) ===")
    log(f"Poll interval: {POLL_INTERVAL}s")

    backoff = 1
    skip_addr: str | None = None  # macOS: a peripheral to skip for one cycle
    while not stop_event.is_set():
        # Apply any pending skip exactly once, then clear it so the next
        # cycle re-tries retrieveConnected (the device may have recovered).
        target = await discover_target(skip_addr=skip_addr)
        skip_addr = None
        if not target:
            log(f"Device not found, retrying in {backoff}s...")
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
            continue

        addr = target if isinstance(target, str) else target.address
        ok = await connect_and_run(target, stop_event)
        if not ok:
            if sys.platform == "darwin":
                # No string cache to drop; instead skip this stale handle on
                # the next retrieveConnected so the scan fallback is reachable.
                skip_addr = addr
            else:
                log("Invalidating cached address")
                SAVED_ADDR_FILE.unlink(missing_ok=True)
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)
        else:
            backoff = 1


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
