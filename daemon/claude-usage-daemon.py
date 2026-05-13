"""Claude Usage Tracker Daemon - Windows port.

Cross-platform Python rewrite of claude-usage-daemon.sh, using bleak for BLE.
Reads the Claude Code OAuth token, polls Anthropic /v1/messages every 60s,
parses anthropic-ratelimit-* response headers, and writes a small JSON
payload to the Clawd-Meter ESP32 over BLE GATT.

Setup:
    pip install bleak
    python claude-usage-daemon.py

Optional env vars:
    DEVICE_MAC      Skip name-based discovery; connect to this MAC directly.
    POLL_INTERVAL   Seconds between API polls (default 60).
    FORGET_DEVICE_ON_SCAN_FAIL
                    On Windows, remove the remembered Bluetooth device by MAC
                    after a failed scan, then retry discovery once.
"""

from __future__ import annotations

import asyncio
import ctypes
import json
import os
import signal
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime
from pathlib import Path

from bleak import BleakClient, BleakScanner
from bleak.backends.device import BLEDevice
from bleak.exc import BleakError

# ---- Protocol constants (must match firmware/src/ble.cpp) ----
DEVICE_NAME = "Claude Controller"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"  # daemon -> device (write)
TX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000003"  # device -> daemon (ack/nack)
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"  # device -> daemon (refresh request)

# ---- Tunables ----
POLL_INTERVAL = int(os.environ.get("POLL_INTERVAL", "60"))
SCAN_TIMEOUT = 10.0
RECONNECT_BACKOFF_MAX = 60
WINDOWS_FORGET_ON_SCAN_FAIL = os.environ.get("FORGET_DEVICE_ON_SCAN_FAIL", "").strip().lower() in {
    "1",
    "true",
    "yes",
    "on",
}

# ---- Paths ----
CREDENTIALS_PATH = Path.home() / ".claude" / ".credentials.json"
CODEX_CREDENTIALS_PATH = Path.home() / ".codex" / "auth.json"
MAC_CACHE_PATH = Path.home() / ".config" / "claude-usage-monitor" / "ble-address"
LOG_FILE_PATH = Path.home() / ".config" / "claude-usage-monitor" / "daemon.log"
LOG_MAX_BYTES = 1_000_000  # rotate at ~1 MB so the log cannot grow forever
ERROR_SUCCESS = 0
ERROR_NOT_FOUND = 1168


def _rotate_log_if_needed() -> None:
    try:
        if LOG_FILE_PATH.exists() and LOG_FILE_PATH.stat().st_size > LOG_MAX_BYTES:
            backup = LOG_FILE_PATH.with_suffix(".log.1")
            if backup.exists():
                backup.unlink()
            LOG_FILE_PATH.rename(backup)
    except OSError:
        pass


def log(msg: str) -> None:
    line = f"[{datetime.now().strftime('%Y-%m-%d %H:%M:%S')}] {msg}"
    # pythonw.exe (used by the scheduled task) has no real stdout - guard so
    # a closed/missing handle does not kill us.
    try:
        print(line, flush=True)
    except (OSError, ValueError):
        pass
    try:
        LOG_FILE_PATH.parent.mkdir(parents=True, exist_ok=True)
        _rotate_log_if_needed()
        with LOG_FILE_PATH.open("a", encoding="utf-8") as f:
            f.write(line + "\n")
    except OSError:
        pass


def read_token() -> str:
    """Pull the OAuth bearer token out of Claude Code's credentials file."""
    with CREDENTIALS_PATH.open("r", encoding="utf-8") as f:
        data = json.load(f)
    return data["claudeAiOauth"]["accessToken"]


def load_cached_mac() -> str | None:
    env_mac = os.environ.get("DEVICE_MAC", "").strip()
    if env_mac:
        return env_mac
    if MAC_CACHE_PATH.exists():
        mac = MAC_CACHE_PATH.read_text(encoding="utf-8").strip()
        return mac or None
    return None


def save_cached_mac(mac: str) -> None:
    MAC_CACHE_PATH.parent.mkdir(parents=True, exist_ok=True)
    MAC_CACHE_PATH.write_text(mac, encoding="utf-8")


def drop_cached_mac() -> None:
    try:
        MAC_CACHE_PATH.unlink()
    except FileNotFoundError:
        pass


def _normalize_mac(mac: str) -> str:
    raw = mac.strip().replace("-", ":").upper()
    parts = raw.split(":")
    if len(parts) != 6 or any(len(part) != 2 for part in parts):
        raise ValueError(f"Invalid Bluetooth MAC address: {mac!r}")
    return ":".join(parts)


def forget_windows_bt_device(mac: str) -> bool:
    """Remove a remembered Windows Bluetooth device by MAC address."""
    if sys.platform != "win32":
        return False

    normalized = _normalize_mac(mac)
    addr_value = int(normalized.replace(":", ""), 16)
    addr = ctypes.c_uint64(addr_value)
    bluetooth_remove_device = ctypes.WinDLL("bthprops.cpl").BluetoothRemoveDevice
    bluetooth_remove_device.argtypes = [ctypes.POINTER(ctypes.c_uint64)]
    bluetooth_remove_device.restype = ctypes.c_uint32

    result = bluetooth_remove_device(ctypes.byref(addr))
    if result == ERROR_SUCCESS:
        log(f"Removed remembered Bluetooth device {normalized}.")
        return True
    if result == ERROR_NOT_FOUND:
        log(f"Bluetooth device {normalized} was not remembered.")
        return False

    log(f"BluetoothRemoveDevice({normalized}) failed with Win32 error {result}.")
    return False


def read_codex_creds() -> tuple[str, str] | None:
    """Return (access_token, account_id) from ~/.codex/auth.json, or None."""
    try:
        with CODEX_CREDENTIALS_PATH.open("r", encoding="utf-8") as f:
            data = json.load(f)
        tokens = data.get("tokens", {})
        access = tokens.get("access_token", "")
        account_id = tokens.get("account_id", "")
        if access and account_id:
            return access, account_id
    except (OSError, KeyError, json.JSONDecodeError):
        pass
    return None


def call_codex(access_token: str, account_id: str) -> dict | None:
    """GET chatgpt.com/backend-api/wham/usage with the Codex OAuth session token."""
    req = urllib.request.Request(
        "https://chatgpt.com/backend-api/wham/usage",
        method="GET",
        headers={
            "Authorization": f"Bearer {access_token}",
            "ChatGPT-Account-Id": account_id,
            "User-Agent": "OpenAI/Codex-CLI",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return json.loads(resp.read())
    except urllib.error.HTTPError as e:
        if e.code == 401:
            log("Codex token expired - re-login with: codex auth login")
        else:
            log(f"Codex API error: {e.code}")
        return None
    except urllib.error.URLError as e:
        log(f"Codex network error: {e.reason}")
        return None


def codex_cx_fields(response: dict) -> dict:
    """Extract cx_* fields from a wham/usage response dict."""
    try:
        pw = response["rate_limit"]["primary_window"]
        sw = response["rate_limit"]["secondary_window"]
        return {
            "cx_s": pw["used_percent"],
            "cx_sr": max(0, pw["reset_after_seconds"] // 60),
            "cx_w": sw["used_percent"],
            "cx_wr": max(0, sw["reset_after_seconds"] // 60),
        }
    except (KeyError, TypeError):
        return {}


def poll_codex() -> dict:
    """Read Codex credentials and hit wham/usage. Returns cx_* dict (may be empty)."""
    creds = read_codex_creds()
    if not creds:
        return {}
    result = call_codex(*creds)
    if result is None:
        return {}
    return codex_cx_fields(result)


def call_anthropic(token: str) -> dict | None:
    """One tiny POST to /v1/messages. We only care about the rate-limit headers."""
    body = json.dumps(
        {
            "model": "claude-haiku-4-5-20251001",
            "max_tokens": 1,
            "messages": [{"role": "user", "content": "hi"}],
        }
    ).encode("utf-8")

    req = urllib.request.Request(
        "https://api.anthropic.com/v1/messages",
        data=body,
        method="POST",
        headers={
            "Authorization": f"Bearer {token}",
            "anthropic-version": "2023-06-01",
            "anthropic-beta": "oauth-2025-04-20",
            "Content-Type": "application/json",
            "User-Agent": "claude-code/2.1.5",
        },
    )

    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            return dict(resp.headers)
    except urllib.error.HTTPError as e:
        # 429 / 401 still carry the rate-limit headers - that is what we want.
        return dict(e.headers) if e.headers else None
    except urllib.error.URLError as e:
        log(f"API error: {e.reason}")
        return None


def headers_to_payload(headers: dict, cx: dict | None = None) -> bytes:
    """Translate Anthropic rate-limit headers (+ optional Codex cx_* fields)."""

    def hget(name: str) -> str:
        # urllib.request lowercases header names on access via dict, but
        # http.client preserves case in some Python versions. Try both.
        for key in headers:
            if key.lower() == name.lower():
                return headers[key]
        return ""

    try:
        u5 = float(hget("anthropic-ratelimit-unified-5h-utilization") or 0)
        r5 = int(float(hget("anthropic-ratelimit-unified-5h-reset") or 0))
        u7 = float(hget("anthropic-ratelimit-unified-7d-utilization") or 0)
        r7 = int(float(hget("anthropic-ratelimit-unified-7d-reset") or 0))
    except ValueError:
        u5 = r5 = u7 = r7 = 0
    status = hget("anthropic-ratelimit-unified-5h-status") or "unknown"

    now = int(time.time())
    payload: dict = {
        "s": round(u5 * 100),
        "sr": max(0, (r5 - now) // 60),
        "w": round(u7 * 100),
        "wr": max(0, (r7 - now) // 60),
        "st": status,
        "ok": True,
    }
    if cx:
        payload.update(cx)
    return json.dumps(payload, separators=(",", ":")).encode("utf-8")


async def find_device() -> BLEDevice | None:
    """Find the ESP32 by cached MAC first, then by advertising name."""
    mac = load_cached_mac()
    if mac:
        log(f"Trying cached MAC {mac}...")
        dev = await BleakScanner.find_device_by_address(mac, timeout=SCAN_TIMEOUT)
        if dev:
            return dev
        log("Cached MAC not seen, falling back to name scan")
        drop_cached_mac()

    log(f"Scanning for '{DEVICE_NAME}'...")
    dev = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=SCAN_TIMEOUT)
    if dev:
        save_cached_mac(dev.address)
        return dev

    if WINDOWS_FORGET_ON_SCAN_FAIL and mac:
        log(f"Scan failed; forgetting remembered Windows Bluetooth device {mac} and retrying once.")
        removed = await asyncio.to_thread(forget_windows_bt_device, mac)
        if removed:
            drop_cached_mac()
            await asyncio.sleep(2)
            log(f"Retrying scan for '{DEVICE_NAME}' after Bluetooth removal...")
            dev = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=SCAN_TIMEOUT)
            if dev:
                save_cached_mac(dev.address)
    return dev


async def run_session(device: BLEDevice, stop_event: asyncio.Event) -> None:
    """One connected session. Returns when disconnected or stop is signalled."""
    refresh_event = asyncio.Event()

    def on_disconnect(_client: BleakClient) -> None:
        log("Disconnected.")
        # Wake the poll loop so it exits promptly.
        stop_event_local.set()

    stop_event_local = asyncio.Event()

    async with BleakClient(device, disconnected_callback=on_disconnect) as client:
        log(f"Connected to {device.address}.")

        # Device-initiated refresh: ESP fires a single byte on REQ char when
        # it has no data yet (fresh boot). We do not care about the value -
        # just the fact that it notified.
        def on_req_notify(_handle: int, _data: bytearray) -> None:
            log("Device requested refresh.")
            refresh_event.set()

        try:
            await client.start_notify(REQ_CHAR_UUID, on_req_notify)
        except BleakError as e:
            log(f"Note: could not subscribe to REQ char ({e}); refresh hints disabled.")

        last_poll = 0.0
        while not stop_event.is_set() and not stop_event_local.is_set():
            do_poll = refresh_event.is_set() or (time.time() - last_poll) >= POLL_INTERVAL
            if do_poll:
                refresh_event.clear()
                try:
                    token = await asyncio.to_thread(read_token)
                except (OSError, KeyError, json.JSONDecodeError) as e:
                    log(f"Could not read credentials: {e}")
                else:
                    headers = await asyncio.to_thread(call_anthropic, token)
                    if headers is not None:
                        cx = await asyncio.to_thread(poll_codex)
                        payload = headers_to_payload(headers, cx or None)
                        log(f"Sending: {payload.decode()}")
                        try:
                            await client.write_gatt_char(RX_CHAR_UUID, payload, response=False)
                            last_poll = time.time()
                        except BleakError as e:
                            log(f"Write failed: {e}")
                            break
            # Short tick so refresh notifies are picked up promptly.
            try:
                await asyncio.wait_for(stop_event_local.wait(), timeout=5)
            except asyncio.TimeoutError:
                pass


async def main_loop() -> None:
    stop_event = asyncio.Event()

    def _shutdown(*_):
        log("Shutdown requested.")
        stop_event.set()

    if sys.platform != "win32":
        loop = asyncio.get_running_loop()
        for sig in (signal.SIGINT, signal.SIGTERM):
            loop.add_signal_handler(sig, _shutdown)
    else:
        signal.signal(signal.SIGINT, _shutdown)

    log("=== Claude Usage Tracker Daemon (Windows / Python) ===")
    log(f"Poll interval: {POLL_INTERVAL}s")

    backoff = 1
    while not stop_event.is_set():
        device = await find_device()
        if device is None:
            log(f"Device not found, retrying in {backoff}s...")
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, RECONNECT_BACKOFF_MAX)
            continue

        backoff = 1
        try:
            await run_session(device, stop_event)
        except BleakError as e:
            log(f"Session error: {e}")
            # If we cannot connect at all, the cached MAC may be stale.
            drop_cached_mac()
        except Exception as e:  # noqa: BLE001
            log(f"Unexpected error: {e!r}")

        if not stop_event.is_set():
            log("Reconnecting in 2s...")
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=2)
            except asyncio.TimeoutError:
                pass

    log("Daemon stopped.")


if __name__ == "__main__":
    try:
        asyncio.run(main_loop())
    except KeyboardInterrupt:
        pass
