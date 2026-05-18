#!/usr/bin/env python3
"""Claude Usage Tracker Daemon (BLE) — macOS port of claude-usage-daemon.sh.

Polls Claude API rate-limit headers and writes a JSON payload to the
ESP32 "Claude Controller" peripheral over a custom GATT service. Uses
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

DEVICE_NAME = "Claude Controller"
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

# Token refresh: piggyback on Claude Code CLI. When the cached accessToken
# is within TOKEN_REFRESH_BUFFER_SECONDS of expiry, spawn the CLI with a
# trivial prompt — the CLI auto-refreshes via OAuth and writes the new
# token back to Keychain / credentials.json, which the next read picks up.
# Cost: one message per refresh (~every 8h on a healthy token).
TOKEN_REFRESH_BUFFER_SECONDS = 120
TOKEN_REFRESH_CMD = "claude"
TOKEN_REFRESH_CMD_ARGS = ("-p", "ping")
TOKEN_REFRESH_TIMEOUT_SECONDS = 60

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


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def _extract_credentials(blob: str) -> tuple[str, int] | None:
    """Pull (accessToken, expiresAtMs) out of a credentials blob.

    Claude Code stores credentials as a JSON object; the blob may also be
    nested ({"claudeAiOauth": {"accessToken": "...", "expiresAt": ...}}).
    Fall back to a regex match so unexpected shapes still work, and
    finally treat the blob as a raw token if nothing else matches.

    expiresAtMs == 0 means "unknown" — callers should skip proactive
    refresh in that case (e.g., raw-token shape with no expiry field).
    """
    blob = blob.strip()
    if not blob:
        return None

    def _coerce_expiry(v: object) -> int:
        try:
            return int(v) if v is not None else 0
        except (TypeError, ValueError):
            return 0

    def _pluck(d: dict) -> tuple[str, int] | None:
        tok = d.get("accessToken")
        if isinstance(tok, str) and tok:
            return tok, _coerce_expiry(d.get("expiresAt"))
        return None

    try:
        data = json.loads(blob)
    except json.JSONDecodeError:
        data = None
    if isinstance(data, dict):
        # direct: {"accessToken": "...", "expiresAt": ...}
        result = _pluck(data)
        if result:
            return result
        # nested: {"claudeAiOauth": {...}}
        for v in data.values():
            if isinstance(v, dict):
                result = _pluck(v)
                if result:
                    return result
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
    if m:
        m2 = re.search(r'"expiresAt"\s*:\s*(\d+)', blob)
        return m.group(1), int(m2.group(1)) if m2 else 0
    # Raw token (no JSON wrapper) — must look plausible (sk-ant-... etc.)
    if re.fullmatch(r"[A-Za-z0-9_\-.~+/=]{20,}", blob):
        return blob, 0
    return None


def _read_credentials_keychain() -> tuple[str, int] | None:
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
    return _extract_credentials(out.stdout)


def _read_credentials_file() -> tuple[str, int] | None:
    try:
        raw = CREDENTIALS_PATH.read_text()
    except OSError as e:
        log(f"Error reading credentials: {e}")
        return None
    return _extract_credentials(raw)


def read_credentials() -> tuple[str, int] | None:
    if sys.platform == "darwin":
        return _read_credentials_keychain()
    return _read_credentials_file()


async def _trigger_cli_refresh() -> bool:
    """Spawn `claude -p ping` to force the CLI to refresh the OAuth token
    and persist it back to Keychain / credentials.json. Returns True if
    the subprocess exited cleanly within the timeout.
    """
    try:
        proc = await asyncio.create_subprocess_exec(
            TOKEN_REFRESH_CMD,
            *TOKEN_REFRESH_CMD_ARGS,
            stdout=asyncio.subprocess.DEVNULL,
            stderr=asyncio.subprocess.PIPE,
        )
    except FileNotFoundError:
        log(f"{TOKEN_REFRESH_CMD} CLI not found on PATH; cannot refresh token")
        return False
    except OSError as e:
        log(f"Failed to spawn {TOKEN_REFRESH_CMD} for token refresh: {e}")
        return False
    try:
        _, stderr = await asyncio.wait_for(
            proc.communicate(), timeout=TOKEN_REFRESH_TIMEOUT_SECONDS
        )
    except asyncio.TimeoutError:
        proc.kill()
        try:
            await proc.wait()
        except Exception:
            pass
        log(f"Token refresh via CLI timed out after {TOKEN_REFRESH_TIMEOUT_SECONDS}s")
        return False
    if proc.returncode != 0:
        snippet = stderr.decode(errors="replace").strip().splitlines()
        tail = snippet[-1][:200] if snippet else ""
        log(f"{TOKEN_REFRESH_CMD} exited {proc.returncode}: {tail}")
        return False
    return True


async def ensure_fresh_token() -> str | None:
    """Read the cached credentials; if the accessToken is within the
    refresh buffer of its expiry (or already expired), trigger the CLI
    to refresh it, then re-read. Returns the (possibly stale) token, or
    None if nothing usable is available.
    """
    creds = read_credentials()
    if creds is None:
        return None
    token, expires_at_ms = creds
    if expires_at_ms <= 0:
        return token  # unknown expiry — can't decide; let the API call judge
    seconds_left = expires_at_ms / 1000.0 - time.time()
    if seconds_left > TOKEN_REFRESH_BUFFER_SECONDS:
        return token
    log(f"Token expires in {int(seconds_left)}s; triggering CLI refresh")
    if not await _trigger_cli_refresh():
        return token  # stale; the upcoming API call will surface the 401
    creds2 = read_credentials()
    if creds2 is None:
        return token
    new_token, new_expires_at_ms = creds2
    if new_expires_at_ms > expires_at_ms:
        new_left_h = (new_expires_at_ms / 1000.0 - time.time()) / 3600.0
        log(f"Token refreshed; new expiry in {new_left_h:+.1f}h")
    else:
        log("CLI returned cleanly but cached expiry did not advance")
    return new_token


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


async def poll_api(token: str) -> dict | None:
    headers = dict(API_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {token}"
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.post(API_URL, headers=headers, json=API_BODY)
    except httpx.HTTPError as e:
        log(f"API call failed: {e}")
        return None
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


async def connect_and_run(address: str, stop_event: asyncio.Event) -> bool:
    """Connect to a known address and poll until disconnected or stopped.

    Returns True if the connection was used successfully (so the caller
    keeps the cached address), False if the connection failed and the
    cache should be invalidated.
    """
    log(f"Connecting to {address}...")
    client = BleakClient(address)
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
                token = await ensure_fresh_token()
                if not token:
                    log("No token; skipping poll")
                else:
                    payload = await poll_api(token)
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
    while not stop_event.is_set():
        address = load_cached_address()
        if not address:
            address = await scan_for_device()
            if address:
                save_address(address)
            else:
                log(f"Device not found, retrying in {backoff}s...")
                try:
                    await asyncio.wait_for(stop_event.wait(), timeout=backoff)
                except asyncio.TimeoutError:
                    pass
                backoff = min(backoff * 2, 60)
                continue

        ok = await connect_and_run(address, stop_event)
        if not ok:
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
