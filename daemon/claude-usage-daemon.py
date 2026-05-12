#!/usr/bin/env python3
"""
Claude Usage Tracker Daemon (BLE) — macOS port.

Reads the Claude Code OAuth token, polls the Anthropic API for rate-limit
headers, and sends a JSON usage payload to the Clawdmeter ESP32 over BLE GATT.

Behaviourally equivalent to daemon/claude-usage-daemon.sh: same UUIDs, cache
path, poll cadence (60s + 5s tick), refresh-on-notify behaviour, and JSON
payload schema. The bash version drives bluez over D-Bus; this version drives
CoreBluetooth (or bluez) via bleak.
"""

import asyncio
import json
import re
import signal
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Optional

from bleak import BleakClient, BleakScanner

DEVICE_NAME = "Claude Controller"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"

POLL_INTERVAL = 60
TICK = 5
SCAN_TIMEOUT = 10

CACHE_FILE = Path.home() / ".config" / "claude-usage-monitor" / "ble-address"
TOKEN_FILE = Path.home() / ".claude" / ".credentials.json"


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def read_token() -> str:
    text = TOKEN_FILE.read_text()
    m = re.search(r'"accessToken":"([^"]+)"', text)
    if not m:
        raise RuntimeError(f"accessToken not found in {TOKEN_FILE}")
    return m.group(1)


def load_cached_address() -> Optional[str]:
    if not CACHE_FILE.exists():
        return None
    val = CACHE_FILE.read_text().strip()
    return val or None


def save_cached_address(addr: str) -> None:
    CACHE_FILE.parent.mkdir(parents=True, exist_ok=True)
    CACHE_FILE.write_text(addr)


def invalidate_cache() -> None:
    try:
        CACHE_FILE.unlink()
    except FileNotFoundError:
        pass


async def find_device():
    cached = load_cached_address()
    if cached:
        log(f"Trying cached address: {cached}")
        device = await BleakScanner.find_device_by_address(cached, timeout=SCAN_TIMEOUT)
        if device:
            return device
        log("Cache stale, scanning by name")
        invalidate_cache()

    log(f"Scanning for '{DEVICE_NAME}'...")
    device = await BleakScanner.find_device_by_filter(
        lambda d, ad: (d.name or "") == DEVICE_NAME,
        timeout=SCAN_TIMEOUT,
    )
    if device:
        save_cached_address(device.address)
        log(f"Found: {device.address}")
    return device


def fetch_usage_headers(token: str) -> Optional[dict]:
    body = json.dumps({
        "model": "claude-haiku-4-5-20251001",
        "max_tokens": 1,
        "messages": [{"role": "user", "content": "hi"}],
    }).encode()
    req = urllib.request.Request(
        "https://api.anthropic.com/v1/messages",
        data=body,
        headers={
            "Authorization": f"Bearer {token}",
            "anthropic-version": "2023-06-01",
            "anthropic-beta": "oauth-2025-04-20",
            "Content-Type": "application/json",
            "User-Agent": "claude-code/2.1.5",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return {k.lower(): v for k, v in resp.headers.items()}
    except urllib.error.HTTPError as e:
        # Rate-limit headers are present on error responses too.
        return {k.lower(): v for k, v in e.headers.items()}
    except (urllib.error.URLError, TimeoutError) as e:
        log(f"API call failed: {e}")
        return None


def build_payload(headers: dict) -> bytes:
    def num(key: str) -> float:
        try:
            return float(headers.get(key, 0))
        except (TypeError, ValueError):
            return 0.0

    s5h_util = num("anthropic-ratelimit-unified-5h-utilization")
    s5h_reset = num("anthropic-ratelimit-unified-5h-reset")
    s7d_util = num("anthropic-ratelimit-unified-7d-utilization")
    s7d_reset = num("anthropic-ratelimit-unified-7d-reset")
    status = headers.get("anthropic-ratelimit-unified-5h-status", "unknown")

    now = time.time()
    payload = {
        "s": round(s5h_util * 100),
        "sr": max(0, round((s5h_reset - now) / 60)),
        "w": round(s7d_util * 100),
        "wr": max(0, round((s7d_reset - now) / 60)),
        "st": status,
        "ok": True,
    }
    return json.dumps(payload, separators=(",", ":")).encode()


async def poll_and_send(client: BleakClient) -> bool:
    try:
        token = read_token()
    except Exception as e:
        log(f"Could not read token: {e}")
        return False

    headers = fetch_usage_headers(token)
    if headers is None:
        return False

    payload = build_payload(headers)
    log(f"Sending: {payload.decode()}")
    try:
        await client.write_gatt_char(RX_CHAR_UUID, payload, response=False)
    except Exception as e:
        log(f"Write failed: {e}")
        return False
    return True


async def run() -> None:
    refresh = asyncio.Event()
    backoff = 1

    def on_req_notify(_sender, _data) -> None:
        refresh.set()

    while True:
        device = await find_device()
        if device is None:
            log(f"Device not found, retrying in {backoff}s...")
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, 60)
            continue

        try:
            async with BleakClient(device) as client:
                log(f"Connected to {device.address}")
                backoff = 1

                try:
                    await client.start_notify(REQ_CHAR_UUID, on_req_notify)
                except Exception as e:
                    log(f"Could not subscribe to refresh char: {e}")

                last_poll = 0.0
                while client.is_connected:
                    now = time.time()
                    if refresh.is_set() or (now - last_poll) >= POLL_INTERVAL:
                        if refresh.is_set():
                            log("Refresh requested by device")
                            refresh.clear()
                        if await poll_and_send(client):
                            last_poll = now
                    await asyncio.sleep(TICK)
        except Exception as e:
            log(f"Connection error: {e}")

        log("Disconnected, reconnecting...")
        await asyncio.sleep(2)


def main() -> None:
    log("=== Claude Usage Tracker Daemon (BLE, macOS) ===")
    log(f"Poll interval: {POLL_INTERVAL}s")

    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    main_task = loop.create_task(run())

    def shutdown() -> None:
        log("Shutting down")
        main_task.cancel()

    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, shutdown)

    try:
        loop.run_until_complete(main_task)
    except asyncio.CancelledError:
        pass
    finally:
        loop.close()


if __name__ == "__main__":
    main()
