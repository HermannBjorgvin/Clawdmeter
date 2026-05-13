#!/usr/bin/env python3
"""
Claude Usage Tracker Daemon (BLE) — macOS port of daemon/claude-usage-daemon.sh.

Reads the Claude Code OAuth token from the macOS login Keychain, hits the
Anthropic API with a 1-token request, parses the rate-limit headers, and
pushes them as JSON to the Clawdmeter ESP32 over BLE GATT.

Run: ~/.claude-usage-daemon-venv/bin/python3 daemon/claude-usage-daemon-macos.py
"""
import asyncio
import json
import os
import re
import signal
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

from bleak import BleakClient, BleakScanner

DEVICE_NAME    = "Claude Controller"
SERVICE_UUID   = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID   = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID  = "4c41555a-4465-7669-6365-000000000004"
POLL_INTERVAL  = 60     # how often to refresh usage data from Anthropic
TICK           = 1      # how often to check the attention state file
SAVED_ADDR     = Path.home() / ".config/claude-usage-monitor/ble-address"
KEYCHAIN_SVC   = "Claude Code-credentials"
STATE_DIR      = Path("/tmp/clawdmeter-sessions")
STALE_AGE      = 24 * 60 * 60   # ignore session files older than this


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def read_token() -> str | None:
    """Pull the OAuth access token out of the login Keychain."""
    try:
        out = subprocess.run(
            ["security", "find-generic-password", "-s", KEYCHAIN_SVC, "-w"],
            capture_output=True, text=True, check=True,
        ).stdout.strip()
    except subprocess.CalledProcessError as e:
        log(f"keychain read failed (rc={e.returncode}); first run will prompt for access")
        return None

    try:
        return json.loads(out)["claudeAiOauth"]["accessToken"]
    except (json.JSONDecodeError, KeyError) as e:
        log(f"unexpected keychain payload shape: {e}")
        return None


def poll_anthropic(token: str) -> dict | None:
    """Make a 1-token request to /v1/messages, parse rate-limit response headers."""
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
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            headers = {k.lower(): v for k, v in resp.headers.items()}
    except urllib.error.HTTPError as e:
        # Rate-limit headers still come back on 4xx — read them.
        headers = {k.lower(): v for k, v in e.headers.items()}
        log(f"API returned {e.code}; using headers anyway")
    except Exception as e:
        log(f"API call failed: {e}")
        return None

    now = int(time.time())

    def get_f(key: str, default: float = 0.0) -> float:
        try: return float(headers.get(key, default))
        except (TypeError, ValueError): return default

    u5 = get_f("anthropic-ratelimit-unified-5h-utilization")
    r5 = get_f("anthropic-ratelimit-unified-5h-reset")
    u7 = get_f("anthropic-ratelimit-unified-7d-utilization")
    r7 = get_f("anthropic-ratelimit-unified-7d-reset")
    status = headers.get("anthropic-ratelimit-unified-5h-status", "unknown")

    return {
        "s":  round(u5 * 100),
        "sr": max(0, round((r5 - now) / 60)) if r5 else 0,
        "w":  round(u7 * 100),
        "wr": max(0, round((r7 - now) / 60)) if r7 else 0,
        "st": status,
        "ok": True,
    }


MAX_SESSIONS_SENT = 6   # firmware screen holds up to this many rows
STATE_SHORT = {"waiting": "w", "working": "k", "idle": "i"}


def read_sessions() -> list[dict]:
    """Load all per-session state files, drop stale ones, sort newest first."""
    if not STATE_DIR.exists():
        return []

    now = time.time()
    out: list[dict] = []
    for f in STATE_DIR.iterdir():
        if not f.is_file() or f.suffix != ".json":
            continue
        try:
            d = json.loads(f.read_text())
        except (OSError, json.JSONDecodeError):
            continue
        ts = int(d.get("ts", 0))
        if (now - ts) > STALE_AGE:
            try: f.unlink()
            except OSError: pass
            continue
        out.append(d)

    out.sort(key=lambda d: d.get("ts", 0), reverse=True)
    return out


def build_payload(sessions: list[dict], usage: dict | None) -> dict:
    """Assemble the JSON blob we'll write to the device.

    Compacts fields aggressively because BLE writes share an MTU budget:
      sess: [{ p: project, s: state-letter, m: message }, ...]
    """
    waiting = [s for s in sessions if s.get("state") == "waiting"]
    if waiting:
        head = waiting[0].get("msg") or "Claude is waiting"
        attn = head if len(waiting) == 1 else f"{len(waiting)} waiting — {head}"
        attn = attn[:90]
    else:
        attn = ""

    sess_arr: list[dict] = []
    for s in sessions[:MAX_SESSIONS_SENT]:
        cwd = s.get("cwd") or ""
        proj = os.path.basename(cwd.rstrip("/")) or "session"
        item = {
            "p": proj[:14],
            "s": STATE_SHORT.get(s.get("state", ""), "i"),
        }
        if s.get("state") == "waiting" and s.get("msg"):
            item["m"] = s["msg"][:40]
        sess_arr.append(item)

    payload = dict(usage) if usage else {
        "s": 0, "sr": 0, "w": 0, "wr": 0, "st": "unknown", "ok": False,
    }
    payload["at"]   = attn
    payload["sess"] = sess_arr
    return payload


def state_mtime() -> float:
    """Directory mtime moves whenever a session file is created/removed."""
    try:
        return STATE_DIR.stat().st_mtime
    except OSError:
        return 0.0


def load_saved_address() -> str | None:
    if not SAVED_ADDR.exists():
        return None
    addr = SAVED_ADDR.read_text().strip()
    if re.fullmatch(r"[0-9A-Fa-f-]{36}|[0-9A-Fa-f:]{17}", addr):
        return addr
    return None


def save_address(addr: str) -> None:
    SAVED_ADDR.parent.mkdir(parents=True, exist_ok=True)
    SAVED_ADDR.write_text(addr)


async def find_device(timeout: float = 10.0):
    """Return a BLEDevice for the Clawdmeter, or None.

    Tries cached address first, then a name scan. On macOS, BLE devices are
    identified by a CoreBluetooth UUID (not MAC), and that UUID is stable
    per (mac-host, peripheral) pair.
    """
    saved = load_saved_address()
    if saved:
        log(f"trying cached address {saved}")
        dev = await BleakScanner.find_device_by_address(saved, timeout=timeout)
        if dev:
            return dev
        log("cached address not seen; falling back to name scan")

    log(f"scanning for '{DEVICE_NAME}'…")
    dev = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=timeout)
    if dev:
        log(f"found {dev.address}")
        save_address(dev.address)
    return dev


async def session_loop(stop: asyncio.Event):
    """One connection's lifetime. Returns when disconnected or stop signaled."""
    dev = await find_device()
    if not dev:
        log("device not found, will retry")
        await asyncio.sleep(5)
        return

    refresh_event = asyncio.Event()
    cached_usage: dict | None = None
    last_payload_str = ""        # only re-send if something actually changed
    last_state_mtime = 0.0

    async with BleakClient(dev) as client:
        log(f"connected to {dev.address}")

        try:
            def _on_req(_char, data: bytearray):
                log(f"refresh requested by device ({bytes(data).hex()})")
                refresh_event.set()
            await client.start_notify(REQ_CHAR_UUID, _on_req)
        except Exception as e:
            log(f"could not subscribe to refresh char: {e}")

        async def send(payload: dict) -> bool:
            blob = json.dumps(payload, separators=(",", ":")).encode()
            log(f"sending {blob.decode()}")
            try:
                await client.write_gatt_char(RX_CHAR_UUID, blob, response=False)
                return True
            except Exception as e:
                log(f"write failed: {e}")
                return False

        last_poll = 0.0
        while client.is_connected and not stop.is_set():
            now = time.time()
            # The device fires a refresh request whenever it's missing data
            # (e.g. fresh boot, post-reflash). Always resend on a refresh,
            # bypassing the skip-if-same dedup below.
            refresh_requested = refresh_event.is_set()
            wants_poll = refresh_requested or (now - last_poll) >= POLL_INTERVAL

            if wants_poll:
                refresh_event.clear()
                token = read_token()
                if token:
                    polled = poll_anthropic(token)
                    if polled is not None:
                        cached_usage = polled
                        last_poll = now
                else:
                    log("no token; will retry next tick")

            cur_mtime = state_mtime()
            mtime_changed = cur_mtime != last_state_mtime

            if wants_poll or mtime_changed:
                sessions = read_sessions()
                payload  = build_payload(sessions, cached_usage)
                payload_str = json.dumps(payload, separators=(",", ":"))
                if refresh_requested or payload_str != last_payload_str:
                    if await send(payload):
                        last_payload_str = payload_str
                        last_state_mtime = cur_mtime
                    else:
                        break
                else:
                    last_state_mtime = cur_mtime

            try:
                await asyncio.wait_for(stop.wait(), timeout=TICK)
            except asyncio.TimeoutError:
                pass

        log("disconnected")


async def main():
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stop.set)

    log("=== Claude Usage Tracker Daemon (BLE, macOS) ===")
    log(f"poll interval: {POLL_INTERVAL}s")

    backoff = 1
    while not stop.is_set():
        try:
            await session_loop(stop)
            backoff = 1
        except Exception as e:
            log(f"session error: {e}; retrying in {backoff}s")
            try:
                await asyncio.wait_for(stop.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 60)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
