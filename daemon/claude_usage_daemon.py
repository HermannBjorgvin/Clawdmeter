#!/usr/bin/env python3
"""Claude Usage Tracker Daemon (USB CDC) — macOS port of claude-usage-daemon.sh.

Polls Claude API rate-limit headers and writes a JSON payload to the
ESP32 over USB CDC serial (default port: /dev/cu.usbmodem*). This is the
`usb-transport` branch's macOS daemon — the `main` branch uses BLE instead.

Why /dev/cu.usbmodem* and not /dev/tty.usbmodem*?
  The `cu.*` (callout) node does NOT assert DTR on open. The `tty.*`
  (dial-in) node does, which would reset the ESP32-S3 every time we
  open the port. We also explicitly clear HUPCL via termios as belt and
  braces in case the kernel ever defaults differently.
"""

import asyncio
import getpass
import glob
import json
import os
import re
import signal
import subprocess
import sys
import termios
import time
from pathlib import Path

import httpx
import serial  # pyserial

POLL_INTERVAL = 60
TICK = 5
BAUD = 115200
PORT_GLOBS = (
    "/dev/cu.usbmodem*",  # macOS preferred
    "/dev/ttyACM*",        # Linux fallback (rarely useful — the bash daemon handles Linux)
)
ENV_PORT = os.environ.get("DEVICE_PORT")  # explicit override wins

# macOS: token lives in Keychain (service "Claude Code-credentials").
# Linux: token lives in ~/.claude/.credentials.json.
KEYCHAIN_SERVICE = "Claude Code-credentials"
CREDENTIALS_PATH = Path.home() / ".claude" / ".credentials.json"

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
        if isinstance(data.get("accessToken"), str):
            return data["accessToken"]
        for v in data.values():
            if isinstance(v, dict) and isinstance(v.get("accessToken"), str):
                return v["accessToken"]
    m = re.search(r'"accessToken"\s*:\s*"([^"]+)"', blob)
    if m:
        return m.group(1)
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


def find_port() -> str | None:
    if ENV_PORT and os.path.exists(ENV_PORT):
        return ENV_PORT
    for pattern in PORT_GLOBS:
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[0]
    return None


def open_port(path: str) -> serial.Serial:
    """Open the CDC port with HUPCL cleared.

    pyserial's `dsrdtr=False` covers the obvious DTR-on-open case, but on
    some kernels HUPCL still drops DTR when the file descriptor closes,
    which the firmware sees as a reset on every reconnect. Stomp it
    explicitly via termios after open.
    """
    ser = serial.Serial(
        path,
        baudrate=BAUD,
        timeout=1.0,        # read timeout (s)
        write_timeout=2.0,
        dsrdtr=False,
        rtscts=False,
        xonxoff=False,
    )
    try:
        fd = ser.fileno()
        attrs = termios.tcgetattr(fd)
        # cflag is index 2; HUPCL is in cflag.
        attrs[2] &= ~termios.HUPCL
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
    except (termios.error, OSError) as e:
        log(f"HUPCL clear failed (non-fatal): {e}")
    return ser


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

    return {
        "s": pct(hdr("anthropic-ratelimit-unified-5h-utilization")),
        "sr": reset_minutes(hdr("anthropic-ratelimit-unified-5h-reset")),
        "w": pct(hdr("anthropic-ratelimit-unified-7d-utilization")),
        "wr": reset_minutes(hdr("anthropic-ratelimit-unified-7d-reset")),
        "st": hdr("anthropic-ratelimit-unified-5h-status", "unknown"),
        "ok": True,
    }


async def reader_task(ser: serial.Serial, refresh_event: asyncio.Event,
                      stop_event: asyncio.Event) -> None:
    """Background task: read lines from the device, set refresh_event on REQ.

    Runs the blocking pyserial readline in a thread executor so it doesn't
    block the asyncio loop. ACK/NACK/READY/etc are logged but otherwise
    not acted on — they're informational.
    """
    loop = asyncio.get_running_loop()
    while not stop_event.is_set():
        try:
            line_bytes = await loop.run_in_executor(None, ser.readline)
        except (serial.SerialException, OSError) as e:
            log(f"Reader: port error: {e}")
            return
        if not line_bytes:
            continue  # timeout, no data
        line = line_bytes.decode(errors="replace").strip()
        if not line:
            continue
        log(f"[device] {line}")
        if line == "REQ":
            refresh_event.set()


async def write_payload(ser: serial.Serial, payload: dict) -> bool:
    data = (json.dumps(payload, separators=(",", ":")) + "\n").encode()
    log(f"Sending: {data.decode().rstrip()}")
    try:
        await asyncio.get_running_loop().run_in_executor(
            None, lambda: (ser.write(data), ser.flush())
        )
        return True
    except (serial.SerialException, OSError) as e:
        log(f"Write failed: {e}")
        return False


async def run_session(port_path: str, stop_event: asyncio.Event) -> None:
    log(f"Opening {port_path}...")
    try:
        ser = open_port(port_path)
    except (serial.SerialException, OSError) as e:
        log(f"Open failed: {e}")
        await asyncio.sleep(2)
        return

    log("Port opened")
    refresh_event = asyncio.Event()
    reader = asyncio.create_task(reader_task(ser, refresh_event, stop_event))

    last_poll = 0.0
    write_fails = 0
    try:
        while not stop_event.is_set():
            now = time.time()
            if refresh_event.is_set() or (now - last_poll) >= POLL_INTERVAL:
                if refresh_event.is_set():
                    log("Refresh requested by device")
                    refresh_event.clear()
                token = read_token()
                if not token:
                    log("No token; skipping poll")
                else:
                    payload = await poll_api(token)
                    if payload is not None:
                        if await write_payload(ser, payload):
                            last_poll = time.time()
                            write_fails = 0
                        else:
                            write_fails += 1
                            if write_fails >= 2:
                                log(f"Write failed {write_fails}x, recycling port")
                                break

            try:
                await asyncio.wait_for(refresh_event.wait(), timeout=TICK)
            except asyncio.TimeoutError:
                pass
    finally:
        reader.cancel()
        try:
            await reader
        except asyncio.CancelledError:
            pass
        try:
            ser.close()
        except (serial.SerialException, OSError):
            pass


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

    log("=== Claude Usage Tracker Daemon (USB CDC, macOS) ===")
    log(f"Poll interval: {POLL_INTERVAL}s")

    backoff = 1
    while not stop_event.is_set():
        port = find_port()
        if not port:
            log(f"No USB CDC port found, retrying in {backoff}s...")
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 30)
            continue

        backoff = 1
        await run_session(port, stop_event)
        if not stop_event.is_set():
            log("Session ended, looking for port again...")
            await asyncio.sleep(1)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
