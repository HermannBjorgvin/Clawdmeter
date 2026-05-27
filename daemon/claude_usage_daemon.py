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
import http.server
import json
import os
import re
import secrets
import signal
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

import httpx
import serial  # pyserial
from serial.tools import list_ports

# termios is Unix-only — used for HUPCL clear on macOS/Linux. Skipped on Windows
# where the kernel doesn't toggle DTR-on-close the same way; pyserial's
# dsrdtr=False covers the open-side case sufficiently.
if sys.platform != "win32":
    import termios

POLL_INTERVAL = 60
TICK = 5
BAUD = 115200
PORT_GLOBS = (
    "/dev/cu.usbmodem*",  # macOS preferred
    "/dev/ttyACM*",        # Linux fallback (rarely useful — the bash daemon handles Linux)
)
ENV_PORT = os.environ.get("DEVICE_PORT")  # explicit override wins

# Activity / hook state — written by daemon/clawdmeter_hook.py on every
# Claude Code hook event, read here on every tick. The wire schema is the
# same compact form PR #22 uses over BLE; USB CDC has no MTU budget so we
# don't enforce a payload cap.
STATE_FILE = Path.home() / ".clawdmeter" / "state.json"
MAX_SESSIONS = 3
MAX_TODOS_PER_SESSION = 10
TODO_CONTENT_MAX = 50
TODO_ACTIVEFORM_MAX = 40
USER_PROMPT_MAX = 60
CURRENT_TOOL_MAX = 24
TOOL_ARGS_MAX = 60
SESSION_STALE_SECONDS = 10 * 60

_STATUS_NUM = {"pending": 0, "in_progress": 1, "completed": 2}
_PHASE_NUM = {"idle": 0, "running": 1}

# ---- WiFi-bridge mode: HTTP server that the firmware can poll over the LAN ----
# Disabled by default to keep behaviour identical to PR #26 when the user
# only wants USB CDC. Set CLAWDMETER_HTTP=1 in the environment to enable.
HTTP_ENABLE = os.environ.get("CLAWDMETER_HTTP", "0") == "1"
HTTP_PORT = int(os.environ.get("CLAWDMETER_HTTP_PORT", "8787"))
HTTP_TOKEN_FILE = Path.home() / ".clawdmeter" / "wifi_token"

# Shared between the asyncio main loop and the synchronous HTTP server thread.
# Holds the most-recently-assembled wire payload so any GET /payload reply is
# instant — no API call or state.json read in the request hot path.
_http_payload_lock = threading.Lock()
_http_payload: dict = {}

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
        ta = str(s.get("current_tool_args", ""))[:TOOL_ARGS_MAX]
        if ta:
            entry["ta"] = ta
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


# ---- HTTP server (WiFi-bridge mode) ----------------------------------------

def ensure_http_token() -> str:
    """Return a persistent bearer token. Generated once, stored in the
    user-home `.clawdmeter` directory. Firmware must be provisioned with the
    same value via captive portal or build flag.
    """
    HTTP_TOKEN_FILE.parent.mkdir(parents=True, exist_ok=True)
    if HTTP_TOKEN_FILE.exists():
        existing = HTTP_TOKEN_FILE.read_text().strip()
        if existing:
            return existing
    token = secrets.token_urlsafe(32)
    HTTP_TOKEN_FILE.write_text(token)
    # POSIX file-permission tightening; harmless on Windows.
    try:
        os.chmod(HTTP_TOKEN_FILE, 0o600)
    except OSError:
        pass
    return token


def lan_ip() -> str:
    """Best-effort discovery of the IP address the firmware should hit.
    Doesn't actually open a connection — just asks the OS routing table
    which interface would carry traffic to a public host.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def update_http_payload(payload: dict) -> None:
    """Called whenever the main loop assembles a fresh payload — both the
    USB-CDC writer and the HTTP server end up serving the same thing."""
    with _http_payload_lock:
        _http_payload.clear()
        _http_payload.update(payload)


class _PayloadHandler(http.server.BaseHTTPRequestHandler):
    expected_token: str = ""

    def _send_json(self, status: int, body: dict) -> None:
        data = json.dumps(body).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)

    def _check_auth(self) -> bool:
        auth = self.headers.get("Authorization", "")
        if not auth.startswith("Bearer "):
            return False
        return secrets.compare_digest(auth[7:].strip(), self.expected_token)

    def do_GET(self) -> None:
        if self.path == "/health":
            self._send_json(200, {"ok": True})
            return
        if self.path != "/payload":
            self._send_json(404, {"err": "not found"})
            return
        if not self._check_auth():
            self._send_json(401, {"err": "unauthorized"})
            return
        with _http_payload_lock:
            payload = dict(_http_payload)
        if not payload:
            self._send_json(503, {"err": "no payload yet"})
            return
        self._send_json(200, payload)

    def log_message(self, fmt, *args) -> None:
        # Quiet the default access log noise — daemon log already captures
        # high-level activity at log() granularity.
        return


def start_http_server(token: str) -> None:
    _PayloadHandler.expected_token = token
    server = http.server.ThreadingHTTPServer(("0.0.0.0", HTTP_PORT), _PayloadHandler)
    t = threading.Thread(target=server.serve_forever, daemon=True)
    t.start()
    log(f"HTTP bridge listening on http://{lan_ip()}:{HTTP_PORT}/payload")


def find_port() -> str | None:
    if ENV_PORT:
        # On Windows ENV_PORT will be a COM name like 'COM3' which doesn't
        # exist as a filesystem path, so trust the user input directly.
        if sys.platform == "win32" or os.path.exists(ENV_PORT):
            return ENV_PORT
    if sys.platform == "win32":
        # Match by USB VID using pyserial's list_ports. Same VID list as
        # flash-win.ps1's auto-detect — Espressif, Silicon Labs CP210x,
        # WCH CH340/CH341, FTDI. This is locale-independent so it works
        # on non-English Windows where driver captions are translated.
        wanted_vids = {0x303A, 0x10C4, 0x1A86, 0x0403}
        for p in sorted(list_ports.comports(), key=lambda x: x.device):
            if p.vid in wanted_vids:
                return p.device  # e.g. 'COM3'
        return None
    for pattern in PORT_GLOBS:
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[0]
    return None


def open_port(path: str) -> serial.Serial:
    """Open the CDC port with DTR-on-open suppressed.

    pyserial's `dsrdtr=False` covers the obvious DTR-on-open case on all
    platforms. On macOS/Linux we additionally clear HUPCL via termios because
    some kernels still drop DTR when the file descriptor closes, which the
    firmware sees as a reset on every reconnect. Windows handles this
    differently — no termios available, dsrdtr=False is sufficient.
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
    if sys.platform != "win32":
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
    # ensure_ascii=False keeps multi-byte UTF-8 chars raw instead of expanding
    # them to 6-byte \uXXXX escapes. Two reasons: (1) smaller payloads — em-dashes
    # and accents would otherwise multiply payload size; (2) ArduinoJson on the
    # firmware parses raw UTF-8 transparently but can struggle with escape
    # sequences in long payloads (we saw "JSON parse error: InvalidInput" on the
    # ~700-byte multi-session form before this fix).
    data = (json.dumps(payload, separators=(",", ":"), ensure_ascii=False) + "\n").encode("utf-8")
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
    last_state_mtime = -1.0
    cached_api: dict | None = None
    write_fails = 0
    try:
        while not stop_event.is_set():
            now = time.time()
            api_due = (
                refresh_event.is_set()
                or (now - last_poll) >= POLL_INTERVAL
                or cached_api is None
            )
            state_mtime = state_file_mtime()
            state_changed = state_mtime != last_state_mtime

            if api_due:
                if refresh_event.is_set():
                    log("Refresh requested by device")
                    refresh_event.clear()
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
                # Sessions data — firmware's parse_json is now two-pass
                # (filtered Usage first, sessions best-effort second), so
                # malformed/oversized session arrays no longer break Usage.
                sessions = load_activity_sessions()
                if sessions:
                    payload["sessions"] = sessions
                # Update the HTTP-bridge cache unconditionally — even if the
                # USB CDC write below fails, GET /payload still serves the
                # latest assembled data. The two transports are independent.
                update_http_payload(payload)
                if await write_payload(ser, payload):
                    last_state_mtime = state_mtime
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


async def run_session_no_port(stop_event: asyncio.Event) -> None:
    """HTTP-only session loop — no serial port, no ACK/NACK round-trip.

    Keeps the API poll + state.json watcher running so the HTTP cache stays
    fresh. Used when CLAWDMETER_HTTP=1 is set and no USB CDC port is found,
    or when the user simply prefers WiFi as the only transport.
    """
    log("HTTP-only mode (no USB CDC port)")
    last_poll = 0.0
    last_state_mtime = -1.0
    cached_api: dict | None = None
    while not stop_event.is_set():
        now = time.time()
        api_due = (
            (now - last_poll) >= POLL_INTERVAL
            or cached_api is None
        )
        state_mtime = state_file_mtime()
        state_changed = state_mtime != last_state_mtime

        if api_due:
            token = read_token()
            if token:
                fresh = await poll_api(token)
                if fresh is not None:
                    cached_api = fresh
                    last_poll = time.time()
            else:
                log("No token; skipping API poll")

        if api_due or state_changed:
            payload = dict(cached_api) if cached_api else {
                "s": 0, "sr": 0, "w": 0, "wr": 0,
                "st": "unknown", "ok": False,
            }
            sessions = load_activity_sessions()
            if sessions:
                payload["sessions"] = sessions
            update_http_payload(payload)
            last_state_mtime = state_mtime

        try:
            await asyncio.wait_for(stop_event.wait(), timeout=TICK)
        except asyncio.TimeoutError:
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

    if HTTP_ENABLE:
        token = ensure_http_token()
        log(f"WiFi-bridge token: {token}")
        log(f"WiFi-bridge token file: {HTTP_TOKEN_FILE}")
        start_http_server(token)
    else:
        # Still drive the periodic poll loop even if there's no USB port,
        # so the HTTP cache stays current when WiFi mode is added later.
        pass

    backoff = 1
    while not stop_event.is_set():
        port = find_port()
        if not port:
            # In HTTP-only mode (no USB port and HTTP enabled) we still want
            # to keep polling the API + state.json so the HTTP cache stays
            # fresh. Run a port-less session loop in that case.
            if HTTP_ENABLE:
                await run_session_no_port(stop_event)
                continue
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
