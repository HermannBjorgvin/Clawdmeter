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

# Activity / hook state — written by daemon/clawdmeter_hook.py on every
# Claude Code hook event, read here on every tick.
STATE_FILE = Path.home() / ".clawdmeter" / "state.json"
MAX_SESSIONS = 3            # most-recently-active sessions sent to device
MAX_TODOS_PER_SESSION = 10  # head of the list, oldest-first ordering preserved
TODO_CONTENT_MAX = 50
TODO_ACTIVEFORM_MAX = 40
SESSION_STALE_SECONDS = 10 * 60   # drop from payload if no activity within this window
# BLE single-attribute write limit per the Core spec is 512 bytes; we leave
# headroom for header/framing variability.
MAX_BLE_PAYLOAD = 480

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


def _read_token_keychain() -> str | None:
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
    return _extract_access_token(out.stdout)


def _read_token_file() -> str | None:
    try:
        raw = CREDENTIALS_PATH.read_text()
    except OSError as e:
        log(f"Error reading credentials: {e}")
        return None
    return _extract_access_token(raw)


def read_token() -> str | None:
    if sys.platform == "darwin":
        return _read_token_keychain()
    return _read_token_file()


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


_STATUS_NUM = {"pending": 0, "in_progress": 1, "completed": 2}
_PHASE_NUM = {"idle": 0, "running": 1}

USER_PROMPT_MAX = 60
CURRENT_TOOL_MAX = 24


def load_activity_sessions() -> list[dict]:
    """Read state.json written by the hook script and project it into the
    compact session schema the firmware expects.

    Returns up to MAX_SESSIONS entries, sorted most-recently-active first,
    each with a head-truncated todos list. activeForm is included only on
    the in-progress item — every other state would never display it.
    """
    try:
        state = json.loads(STATE_FILE.read_text())
    except (OSError, json.JSONDecodeError):
        return []
    sessions = state.get("sessions") or {}
    if not isinstance(sessions, dict):
        return []
    now = time.time()
    fresh = []
    for sid, s in sessions.items():
        if not isinstance(s, dict):
            continue
        la_ts = s.get("last_active_ts") or 0
        age = now - la_ts
        if age > SESSION_STALE_SECONDS:
            continue
        fresh.append((la_ts, s))
    fresh.sort(key=lambda x: x[0], reverse=True)
    out = []
    for la_ts, s in fresh[:MAX_SESSIONS]:
        todos = s.get("todos") or []
        compact_todos = []
        for t in todos[:MAX_TODOS_PER_SESSION]:
            if not isinstance(t, dict):
                continue
            sn = _STATUS_NUM.get(str(t.get("status", "pending")), 0)
            entry = {
                "c": str(t.get("content", ""))[:TODO_CONTENT_MAX],
                "s": sn,
            }
            if sn == 1:
                af = str(t.get("activeForm", ""))[:TODO_ACTIVEFORM_MAX]
                if af:
                    entry["a"] = af
            compact_todos.append(entry)
        entry = {
            "p": str(s.get("project", ""))[:24],
            "m": str(s.get("model", ""))[:24],
            "la": max(0, int(now - la_ts)),
            "ph": _PHASE_NUM.get(str(s.get("phase", "idle")), 0),
            "td": compact_todos,
        }
        ct = str(s.get("current_tool", ""))[:CURRENT_TOOL_MAX]
        if ct:
            entry["t"] = ct
        up = str(s.get("last_user_prompt", ""))[:USER_PROMPT_MAX]
        if up:
            entry["u"] = up
        out.append(entry)
    return out


def state_file_mtime() -> float:
    try:
        return STATE_FILE.stat().st_mtime
    except OSError:
        return 0.0


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
        data = _serialize_capped(payload).encode()
        # response=True (Write Request, not Write Command) lets bleak fall
        # back to ATT Long Write for payloads up to the 512-byte spec cap.
        # Without this, payloads larger than MTU-3 would be silently split
        # into multiple onWrite callbacks on the firmware side.
        try:
            log(f"Sending {len(data)}B: "
                f"{data[:200].decode('utf-8', errors='replace')}{'…' if len(data) > 200 else ''}")
            await self.client.write_gatt_char(RX_CHAR_UUID, data, response=True)
            return True
        except BleakError as e:
            log(f"Write failed: {e}")
            return False


def _serialize_capped(payload: dict) -> str:
    """JSON-serialize payload, shrinking the sessions array if needed so the
    encoded length stays under MAX_BLE_PAYLOAD bytes.

    Graduated trim from the tail-most session, in order of least UX impact:
      1. drop `u` (user prompt) — long-ish, usually optional
      2. drop tail todos one at a time
      3. drop `t` (current_tool) — short, but signal-bearing
      4. drop the whole session
    Repeat across remaining sessions, finally drop `sessions` entirely if
    even an empty list doesn't fit.
    """
    work = json.loads(json.dumps(payload))  # cheap deep copy

    def encode():
        # ensure_ascii=False keeps UTF-8 multi-byte chars as their 2-3
        # byte form instead of 6-byte \uXXXX escapes — saves ~50% on
        # payloads with CJK or other non-Latin user prompts. ArduinoJson
        # on the firmware side decodes UTF-8 transparently.
        return json.dumps(work, separators=(",", ":"), ensure_ascii=False)

    def encoded_bytes():
        # Compare against the BLE byte budget — Python `str` length
        # counts Unicode code points, which under-counts CJK by 3x
        # because each char is 3 bytes in UTF-8.
        return len(encode().encode("utf-8"))

    if encoded_bytes() <= MAX_BLE_PAYLOAD:
        return encode()
    sessions = work.get("sessions") or []
    while sessions:
        last = sessions[-1]
        # Step 1: drop the optional user prompt first
        if "u" in last:
            last.pop("u", None)
        # Step 2: drop tail todos one at a time
        elif last.get("td"):
            last["td"] = last["td"][:-1]
        # Step 3: drop current_tool (still keep project+model+phase
        # so the user sees the session exists)
        elif "t" in last:
            last.pop("t", None)
        # Step 4: drop the whole session
        else:
            sessions.pop()
        work["sessions"] = sessions
        if encoded_bytes() <= MAX_BLE_PAYLOAD:
            return encode()
    work.pop("sessions", None)
    return encode()


async def connect_and_run(address: str, stop_event: asyncio.Event) -> bool:
    """Connect to a known address and poll until disconnected or stopped.

    Two independent push triggers run on the same TICK loop:
      * 60s API poll → refresh rate-limit fields
      * state.json mtime change → forward latest hook-collected sessions

    Whenever either triggers, we resend the merged payload (api fields +
    sessions) so the firmware always has a coherent view.

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

    cached_api: dict | None = None
    last_poll = 0.0
    last_state_mtime = -1.0
    used_successfully = False
    try:
        while client.is_connected and not stop_event.is_set():
            now = time.time()
            elapsed = now - last_poll
            api_due = session.refresh_requested.is_set() or elapsed >= POLL_INTERVAL or cached_api is None
            state_mtime = state_file_mtime()
            state_changed = state_mtime != last_state_mtime

            if api_due:
                session.refresh_requested.clear()
                token = read_token()
                if not token:
                    log("No token; skipping API poll")
                else:
                    fresh = await poll_api(token)
                    if fresh is not None:
                        cached_api = fresh
                        last_poll = time.time()

            if api_due or state_changed:
                payload = dict(cached_api) if cached_api else {
                    "s": 0, "sr": 0, "w": 0, "wr": 0,
                    "st": "unknown", "ok": False,
                }
                sessions = load_activity_sessions()
                if sessions:
                    payload["sessions"] = sessions
                if await session.write_payload(payload):
                    last_state_mtime = state_mtime
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
