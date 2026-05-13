"""Claude Usage Tracker Daemon — Windows port.

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
"""

from __future__ import annotations

import asyncio
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
DEVICE_NAME    = "Claude Controller"
SERVICE_UUID   = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID   = "4c41555a-4465-7669-6365-000000000002"  # daemon → device (write)
TX_CHAR_UUID   = "4c41555a-4465-7669-6365-000000000003"  # device → daemon (ack/nack)
REQ_CHAR_UUID  = "4c41555a-4465-7669-6365-000000000004"  # device → daemon (refresh request)

# ---- Tunables ----
POLL_INTERVAL = int(os.environ.get("POLL_INTERVAL", "60"))
SCAN_TIMEOUT  = 10.0
RECONNECT_BACKOFF_MAX = 60

# ---- Paths ----
CREDENTIALS_PATH = Path.home() / ".claude" / ".credentials.json"
MAC_CACHE_PATH   = Path.home() / ".config" / "claude-usage-monitor" / "ble-address"
LOG_FILE_PATH    = Path.home() / ".config" / "claude-usage-monitor" / "daemon.log"
LOG_MAX_BYTES    = 1_000_000  # rotate at ~1 MB so the log can't grow forever


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
    # pythonw.exe (used by the scheduled task) has no real stdout — guard so
    # a closed/missing handle doesn't kill us.
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


def call_anthropic(token: str) -> dict | None:
    """One tiny POST to /v1/messages. We don't care about the body — only the
    rate-limit response headers."""
    body = json.dumps({
        "model": "claude-haiku-4-5-20251001",
        "max_tokens": 1,
        "messages": [{"role": "user", "content": "hi"}],
    }).encode("utf-8")

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
        # 429 / 401 still carry the rate-limit headers — that's what we want.
        return dict(e.headers) if e.headers else None
    except urllib.error.URLError as e:
        log(f"API error: {e.reason}")
        return None


def headers_to_payload(headers: dict) -> bytes:
    """Translate Anthropic rate-limit headers into the firmware's JSON shape."""
    def hget(name: str) -> str:
        # urllib.request lowercases header names on access via dict, but
        # http.client preserves case in some Python versions. Try both.
        for key in headers:
            if key.lower() == name.lower():
                return headers[key]
        return ""

    try:
        u5 = float(hget("anthropic-ratelimit-unified-5h-utilization") or 0)
        r5 = int(float(hget("anthropic-ratelimit-unified-5h-reset")    or 0))
        u7 = float(hget("anthropic-ratelimit-unified-7d-utilization") or 0)
        r7 = int(float(hget("anthropic-ratelimit-unified-7d-reset")    or 0))
    except ValueError:
        u5 = r5 = u7 = r7 = 0
    status = hget("anthropic-ratelimit-unified-5h-status") or "unknown"

    now = int(time.time())
    payload = {
        "s":  round(u5 * 100),
        "sr": max(0, (r5 - now) // 60),
        "w":  round(u7 * 100),
        "wr": max(0, (r7 - now) // 60),
        "st": status,
        "ok": True,
    }
    return json.dumps(payload, separators=(",", ":")).encode("utf-8")


async def find_device() -> BLEDevice | None:
    """Find the ESP32 by cached MAC first, then by advertising name."""
    mac = load_cached_mac()
    if mac:
        log(f"Trying cached MAC {mac}…")
        dev = await BleakScanner.find_device_by_address(mac, timeout=SCAN_TIMEOUT)
        if dev:
            return dev
        log("Cached MAC not seen, falling back to name scan")
        drop_cached_mac()

    log(f"Scanning for '{DEVICE_NAME}'…")
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
        # it has no data yet (fresh boot). We don't care about the value —
        # just the fact that it notified.
        def on_req_notify(_handle: int, _data: bytearray) -> None:
            log("Device requested refresh.")
            refresh_event.set()

        try:
            await client.start_notify(REQ_CHAR_UUID, on_req_notify)
        except BleakError as e:
            log(f"Note: couldn't subscribe to REQ char ({e}); refresh hints disabled.")

        last_poll = 0.0
        while not stop_event.is_set() and not stop_event_local.is_set():
            do_poll = refresh_event.is_set() or (time.time() - last_poll) >= POLL_INTERVAL
            if do_poll:
                refresh_event.clear()
                try:
                    token = await asyncio.to_thread(read_token)
                except (OSError, KeyError, json.JSONDecodeError) as e:
                    log(f"Couldn't read credentials: {e}")
                else:
                    headers = await asyncio.to_thread(call_anthropic, token)
                    if headers is not None:
                        payload = headers_to_payload(headers)
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
            log(f"Device not found, retrying in {backoff}s…")
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
            # If we can't connect at all, the cached MAC may be stale.
            drop_cached_mac()
        except Exception as e:  # noqa: BLE001
            log(f"Unexpected error: {e!r}")

        if not stop_event.is_set():
            log("Reconnecting in 2s…")
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
