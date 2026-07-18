#!/usr/bin/env python3
"""Claude Usage Tracker Daemon (BLE) — macOS port of claude-usage-daemon.sh.

Polls Claude API rate-limit headers and writes a JSON payload to the
ESP32 "Clawdmeter" peripheral over a custom GATT service. Uses
bleak (CoreBluetooth backend on macOS).
"""

import asyncio
import calendar
import datetime
import getpass
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

import httpx
from bleak import BleakClient
from bleak.exc import BleakError

DEVICE_NAME = "Clawdmeter"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"

POLL_INTERVAL = 60
TICK = 5
CONNECT_TIMEOUT = 20.0

# macOS: token lives in Keychain (service "Claude Code-credentials").
# Linux: token lives in ~/.claude/.credentials.json.
KEYCHAIN_SERVICE = "Claude Code-credentials"
DEFAULT_CONFIG_DIR = Path.home() / ".claude"
STATE_DIR = Path.home() / ".config" / "claude-usage-monitor"
SAVED_ADDR_FILE = STATE_DIR / "ble-address"
CONFIG_FILE = STATE_DIR / "config"
# Attention flag: a Claude Code hook writes an event type into this file when
# Claude needs the user. The connected loop picks it up within one TICK and
# forwards it as "n":"<type>" so the firmware plays the matching melody/view.
# Types: input (waiting for an answer), perm (permission prompt), done (turn
# finished), clear (user is back — dismiss the attention view, no sound).
# Stale flags (older than ATTN_MAX_AGE) are discarded so a flag written while
# the daemon was down doesn't chime hours later.
ATTN_FILE = STATE_DIR / "attention"
ATTN_MAX_AGE = 60
ATTN_TYPES = ("input", "perm", "done", "clear")


def read_attention_flag() -> tuple[str | None, str]:
    """Consume the hook's attention flag → (type, project), or (None, "").

    Stale flags (older than ATTN_MAX_AGE) are discarded so a flag written
    while the daemon was down doesn't chime hours later.
    """
    if not ATTN_FILE.exists():
        return None, ""
    try:
        fresh = (time.time() - ATTN_FILE.stat().st_mtime) <= ATTN_MAX_AGE
        lines = ATTN_FILE.read_text().splitlines()
        ATTN_FILE.unlink(missing_ok=True)
        if fresh and lines:
            kind = lines[0].strip().lower()
            kind = kind if kind in ATTN_TYPES else "input"
            project = lines[1].strip()[:48] if len(lines) > 1 else ""
            return kind, project
    except OSError:
        pass
    return None, ""


# ── Calendar reminders ──────────────────────────────────────────────────────
# Optional: `cal_ics_url = <источник>` in CONFIG_FILE turns on meeting
# reminders. Sources: an https ICS feed (Outlook/Google "publish calendar"),
# an absolute path to a local .ics, or the literal `eventkit` — the macOS
# system calendar read via the signed `calnext` helper (build_calnext.sh;
# plain python can't show the TCC calendar prompt, a binary with an embedded
# Info.plist can). Events are refetched every CAL_FETCH_S with recurring
# events expanded; when a meeting start crosses a `cal_remind` threshold the
# connected loop sends an "n":"cal" payload with "np" = "HH:MM <title>".
# Only the smallest active threshold fires (a daemon restart 7' before a
# meeting sends the 5' reminder once, not the 15' one too), and a slot is
# marked sent only after the BLE write succeeds so a dropped connection
# doesn't eat the reminder.

CAL_FETCH_S = 300
CAL_HORIZON_S = 26 * 3600   # keep in sync with the horizon in calnext.swift
CAL_THRESHOLDS_MIN = (15, 5)
CALNEXT_BIN = Path(__file__).resolve().parent / "calnext"

_cal_events: list[tuple[float, str]] = []   # (start_epoch, title), sorted
_cal_fetched = 0.0
_cal_sent: set[tuple[float, int]] = set()   # (start_epoch, threshold_min)
_cal_err_logged = 0.0


def read_cal_config() -> tuple[str, tuple[int, ...]]:
    """(ics_url, reminder thresholds in minutes); url "" = feature off."""
    url = ""
    thresholds = CAL_THRESHOLDS_MIN
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                key, val = key.strip().lower(), val.strip()
                if key == "cal_ics_url":
                    url = val
                elif key == "cal_remind":
                    mins = sorted({int(m) for m in re.split(r"[,\s]+", val)
                                   if m.isdigit() and int(m) > 0}, reverse=True)
                    if mins:
                        thresholds = tuple(mins)
    except OSError:
        pass
    return url, thresholds


async def refresh_cal_events(url: str) -> None:
    """Refetch the ICS feed into _cal_events if CAL_FETCH_S elapsed.

    Failures only log (rate-limited): the calendar is an optional extra and
    must never take down or delay the usage loop beyond this one await.
    """
    global _cal_events, _cal_fetched, _cal_sent, _cal_err_logged
    if time.time() - _cal_fetched < CAL_FETCH_S:
        return
    _cal_fetched = time.time()   # even on failure — don't hammer every TICK
    try:
        if url == "eventkit":
            events = await asyncio.to_thread(_events_from_eventkit)
        else:
            events = await _events_from_ics(url)
        _cal_events = sorted(events)
        _cal_sent = {(s, m) for (s, m) in _cal_sent if s > time.time() - 3600}
    except Exception as e:  # noqa: BLE001 — any parse/net/tz problem: log & keep going
        if time.time() - _cal_err_logged > 600:
            _cal_err_logged = time.time()
            log(f"Calendar fetch failed: {e}")


def _events_from_eventkit() -> set[tuple[float, str]]:
    """macOS system calendar via the signed calnext helper (blocking).

    130s timeout: the very first run blocks on the TCC permission dialog
    (helper-side cap is 120s); every later run returns instantly.
    """
    proc = subprocess.run([str(CALNEXT_BIN)], capture_output=True, timeout=130)
    if proc.returncode != 0:
        err = proc.stderr.decode(errors="replace").strip()
        raise RuntimeError(err or f"calnext rc={proc.returncode}")
    return {(float(ev["start"]), " ".join(str(ev["title"]).split()) or "Встреча")
            for ev in json.loads(proc.stdout)}


async def _events_from_ics(url: str) -> set[tuple[float, str]]:
    """ICS feed (https or absolute local path) with recurrence expanded."""
    import icalendar
    import recurring_ical_events
    if url.startswith("/"):
        raw = await asyncio.to_thread(Path(url).read_bytes)
    else:
        resp = await _http().get(url, follow_redirects=True)
        resp.raise_for_status()
        raw = resp.content
    cal = icalendar.Calendar.from_ical(raw)
    now = datetime.datetime.now(datetime.timezone.utc)
    horizon = now + datetime.timedelta(seconds=CAL_HORIZON_S)
    events = set()
    for ev in recurring_ical_events.of(cal).between(now, horizon):
        start = ev.get("DTSTART")
        # date (not datetime) = all-day entry — nothing to remind about
        if start is None or not isinstance(start.dt, datetime.datetime):
            continue
        if str(ev.get("STATUS", "")).upper() == "CANCELLED":
            continue
        title = " ".join(str(ev.get("SUMMARY", "")).split()) or "Встреча"
        events.add((start.dt.timestamp(), title))
    return events


def check_cal_reminder(thresholds: tuple[int, ...]) -> tuple[str, float, int] | None:
    """First unsent reminder due now → ("HH:MM title", start, threshold_min).

    Doesn't mark anything sent — call cal_mark_sent() after the payload is
    actually delivered. Concurrent meetings resolve one per TICK.
    """
    now = time.time()
    for start, title in _cal_events:
        if start <= now:
            continue
        active = [m for m in thresholds if start - m * 60 <= now]
        if not active:
            continue
        m = min(active)   # closest threshold wins; larger ones are stale
        if (start, m) in _cal_sent:
            continue
        text = f"{datetime.datetime.fromtimestamp(start):%H:%M} {title}"
        # 48 chars = the firmware's context-line budget (two wrapped lines)
        return text[:48], start, m
    return None


def cal_mark_sent(start: float, threshold: int, thresholds: tuple[int, ...]) -> None:
    """Mark this reminder slot and every larger (already-stale) one sent."""
    _cal_sent.update((start, m) for m in thresholds if m >= threshold)


# Per-session liveness files written by the Claude Code hooks: mtime = last
# heartbeat (PostToolUse / prompt), content "fg" (in a turn) or "bg" (turn
# yielded to a long background task — builds, monitors — heartbeats pause).
# fg entries expire after SESS_TTL_FG; bg entries have no TTL — they're
# verified live via lsof/mtime instead. Count → "a" field → the device's
# working/idle creature and the "·N" session counter.
SESS_DIR = STATE_DIR / "sessions"
SESS_TTL_FG = 4 * 60


def _bg_session_still_running(sid: str) -> bool:
    """Is a bg-marked session's background task really still running?

    A running shell task keeps its .output file open in the session's tasks
    dir — lsof sees that. (Its exit code is useless: 1 even with matches;
    test stdout instead.) Async agents don't hold their transcripts open —
    the main process appends them in bursts — so fresh writes count as
    running work too. Neither = the session died or never came back.
    Same heuristic as has_running_tasks in tools/claude-attention-hook.sh —
    keep the two in sync.
    """
    root = Path(f"/private/tmp/claude-{os.getuid()}")
    now = time.time()
    try:
        for tasks_dir in root.glob(f"*/{sid}/tasks"):
            out = subprocess.run(
                ["lsof", "-w", "+d", str(tasks_dir)],
                capture_output=True, text=True, timeout=10,
            ).stdout
            if out.strip():
                return True
            for f in tasks_dir.iterdir():
                try:
                    if now - f.stat().st_mtime <= 90:
                        return True
                except OSError:
                    continue
    except (OSError, subprocess.SubprocessError):
        pass
    return False


def count_active_sessions() -> int:
    now = time.time()
    n = 0
    try:
        for f in SESS_DIR.iterdir():
            try:
                age = now - f.stat().st_mtime
                mode = f.read_text().strip()
                if mode == "bg":
                    # Trust, but verify: count only while the background task
                    # is genuinely alive; drop ghosts on the spot.
                    if _bg_session_still_running(f.name):
                        n += 1
                    else:
                        f.unlink(missing_ok=True)
                elif age <= SESS_TTL_FG:
                    n += 1
            except OSError:
                continue
    except OSError:
        pass
    return n

API_URL = "https://api.anthropic.com/v1/messages"
# Read-only usage endpoint (what Claude Code's /usage renders). Preferred
# source: unlike the /v1/messages probe it consumes no usage at all.
USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
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


def read_config_dirs() -> list[Path]:
    """Claude config dirs to poll, from the `config_dirs` option (comma list).

    Defaults to [~/.claude] so existing single-plan setups are unchanged. ~ is
    expanded. Mirrors the Linux bash daemon's read_config_dirs.
    """
    raw = ""
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == "config_dirs":
                    raw = val.strip()
    except OSError:
        pass
    if not raw:
        return [DEFAULT_CONFIG_DIR]
    dirs = [Path(p.strip()).expanduser() for p in raw.split(",") if p.strip()]
    return dirs or [DEFAULT_CONFIG_DIR]


def read_token_for(config_dir: Path) -> str | None:
    """Read the OAuth token for one config dir.

    Linux: each dir keeps its own ``<dir>/.credentials.json``. macOS: the default
    install stores the token in Keychain with no file, so for the default dir we
    fall back to Keychain when no file is present — preserving existing
    single-plan macOS behavior. Additional macOS dirs are read from their files;
    a work plan whose token lives only in the single Keychain entry can't be told
    apart there (documented follow-up).
    """
    cred = config_dir / ".credentials.json"
    try:
        if cred.exists():
            return _extract_access_token(cred.read_text())
    except OSError as e:
        log(f"Error reading credentials in {config_dir}: {e}")
    if sys.platform == "darwin" and config_dir == DEFAULT_CONFIG_DIR:
        return _read_token_keychain()
    return None


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
    """Return a BLEDevice for a system-connected 'Clawdmeter', or None.

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

    The daemon only ever targets the device this system already holds — it
    never scans for a nearby device by name, so it can't grab a stranger's or
    the wrong nearby unit. On macOS that's the system-connected peripheral (the
    firmware advertises as an HID keyboard, so once paired the OS auto-connects
    and holds it — HID-grabbed devices are invisible to scans anyway). On other
    platforms it's a previously-pinned address in the cache file. If the device
    isn't held/pinned, we log and wait rather than scanning. ``skip_addr`` skips
    a peripheral whose handle just failed to connect.
    """
    if sys.platform == "darwin":
        dev = await retrieve_connected_macos(skip_addr=skip_addr)
        if dev is None:
            log("Device not held by OS; waiting (not scanning by name)")
        return dev

    address = load_cached_address()
    if not address:
        log("No pinned address cached; waiting (not scanning by name)")
    return address


def read_chime_setting() -> str:
    """Read the `chime` option from the config file. One of: off|on.

    Defaults to "off" (the device stays silent) so existing setups are
    unaffected until the user opts in.
    """
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == "chime":
                    val = val.strip().lower()
                    if val in ("off", "on"):
                        return val
    except OSError:
        pass
    return "off"


def read_clock_setting() -> str:
    """Read the `clock` option from the config file. One of: off|auto|12|24.

    Defaults to "off" (no clock; the device keeps showing "Usage") so existing
    setups are unaffected until the user opts in.
    """
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == "clock":
                    val = val.strip().lower()
                    if val in ("off", "auto", "12", "24"):
                        return val
    except OSError:
        pass
    return "off"


def add_chime_field(payload: dict) -> None:
    """Add "c":1 to the payload when the config opts in, so the firmware may
    sound the session-reset chime. Omitted entirely when chime is off."""
    if read_chime_setting() == "on":
        payload["c"] = 1


def detect_hour_format() -> int:
    """Best-effort 12h/24h detection for the host. Returns 12 or 24 (default 24)."""
    # macOS: the explicit System Settings toggle lives in NSGlobalDomain.
    for key, result in (("AppleICUForce24HourTime", 24), ("AppleICUForce12HourTime", 12)):
        try:
            out = subprocess.run(["defaults", "read", "-g", key],
                                 capture_output=True, text=True, timeout=3)
            if out.stdout.strip() == "1":
                return result
        except (OSError, subprocess.SubprocessError):
            pass
    # Fallback to the C locale's time format (may be C/24h under launchd).
    try:
        import locale
        locale.setlocale(locale.LC_TIME, "")
        fmt = locale.nl_langinfo(locale.T_FMT)
        if "%p" in fmt or "%r" in fmt or "%I" in fmt:
            return 12
    except (ImportError, locale.Error, AttributeError):
        pass
    return 24


def add_clock_fields(payload: dict) -> None:
    """Add wall-clock fields to the payload when the config opts in.

    "t"  = local wall-clock epoch (UTC epoch shifted by the tz offset) so the
           device can show the time without an RTC.
    "tf" = 12 or 24, the hour format the device should render.
    """
    clock = read_clock_setting()
    if clock == "off":
        return
    tf = 24 if clock == "24" else 12 if clock == "12" else detect_hour_format()
    payload["t"] = int(time.time()) + time.localtime().tm_gmtoff
    payload["tf"] = tf


# Poll error codes surfaced to the device as the "err" payload field so it
# can say WHY the data went stale: "auth" (401/403 — token expired; Claude
# Code will refresh it next time the user opens it, we deliberately never
# touch the refresh token ourselves — rotation could log the CLI out),
# "rate" (429), "net", "http".
def _err_from_status(status: int) -> str:
    if status in (401, 403):
        return "auth"
    if status == 429:
        return "rate"
    return "http"


_HTTP: httpx.AsyncClient | None = None


def _http() -> httpx.AsyncClient:
    """Shared keep-alive client — both pollers hit the same host every
    minute; a fresh TLS handshake per poll would be pure waste."""
    global _HTTP
    if _HTTP is None:
        _HTTP = httpx.AsyncClient(timeout=20.0)
    return _HTTP


async def poll_api(token: str) -> dict | str:
    """Fetch usage: the free read-only endpoint first, the /v1/messages probe
    as a fallback (it costs a 1-token Haiku call but knows every account shape
    the headers describe, e.g. Enterprise overage). Returns the payload, or an
    error code string when both sources failed."""
    result = await _poll_usage_endpoint(token)
    if isinstance(result, str):
        result = await _poll_probe(token)   # the probe's verdict is fresher
    if isinstance(result, str):
        return result
    add_chime_field(result)   # adds "c":1 iff the config opts in
    add_clock_fields(result)  # adds "t" + "tf" iff the config opts in
    return result


async def _poll_usage_endpoint(token: str) -> dict | str:
    """GET /api/oauth/usage → payload, or an error code on any surprise
    (HTTP error, unfamiliar response shape, Enterprise-style account) so the
    caller can fall back to the probe."""
    headers = {
        "Authorization": f"Bearer {token}",
        "anthropic-beta": API_HEADERS_TEMPLATE["anthropic-beta"],
        "User-Agent": API_HEADERS_TEMPLATE["User-Agent"],
    }
    try:
        resp = await _http().get(USAGE_URL, headers=headers)
    except httpx.HTTPError as e:
        log(f"Usage endpoint failed: {e}")
        return "net"
    if resp.status_code != 200:
        log(f"Usage endpoint HTTP {resp.status_code}: {resp.text[:200]}")
        return _err_from_status(resp.status_code)

    def mins_until(iso: str | None) -> int:
        if not iso:
            return 0
        try:
            dt = datetime.datetime.fromisoformat(iso)
        except ValueError:
            return 0
        mins = (dt - datetime.datetime.now(datetime.timezone.utc)).total_seconds() / 60.0
        return int(round(mins)) if mins > 0 else 0

    def local_reset_at(iso: str | None) -> str:
        if not iso:
            return ""
        try:
            return _fmt_reset_at(datetime.datetime.fromisoformat(iso))
        except ValueError:
            return ""

    try:
        data = resp.json()
        five, seven = data.get("five_hour"), data.get("seven_day")
        if not isinstance(five, dict) or not isinstance(seven, dict):
            return "http"   # not the Pro/Max shape — let the probe classify it
        status = "allowed"
        for lim in data.get("limits") or []:
            if isinstance(lim, dict) and lim.get("kind") == "session":
                sev = lim.get("severity")
                status = "allowed" if sev == "normal" else str(sev or "unknown")
                break
        return {
            "s": int(round(float(five.get("utilization") or 0))),
            "sr": mins_until(five.get("resets_at")),
            "srt": local_reset_at(five.get("resets_at")),
            "w": int(round(float(seven.get("utilization") or 0))),
            "wr": mins_until(seven.get("resets_at")),
            "wrt": local_reset_at(seven.get("resets_at")),
            "st": status,
            "acct": "pro",
            "ok": True,
        }
    except (ValueError, TypeError) as e:
        log(f"Usage endpoint parse error: {e}")
        return "http"


_WEEKDAYS_RU = ("пн", "вт", "ср", "чт", "пт", "сб", "вс")


def _fmt_reset_at(dt: datetime.datetime) -> str:
    """Wall-clock reset time for the display: "21:00" today, "ср 19:00" on
    another day."""
    dt = dt.astimezone()
    hm = dt.strftime("%H:%M")
    if dt.date() == datetime.datetime.now().astimezone().date():
        return hm
    return f"{_WEEKDAYS_RU[dt.weekday()]} {hm}"


async def _poll_probe(token: str) -> dict | str:
    headers = dict(API_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {token}"
    try:
        resp = await _http().post(API_URL, headers=headers, json=API_BODY)
    except httpx.HTTPError as e:
        log(f"API call failed: {e}")
        return "net"
    if resp.status_code >= 400:
        log(f"API HTTP {resp.status_code}: {resp.text[:200]}")
        return _err_from_status(resp.status_code)

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

    def reset_at(reset_ts: str) -> str:
        try:
            r = float(reset_ts)
        except ValueError:
            return ""
        if r <= now:
            return ""
        return _fmt_reset_at(datetime.datetime.fromtimestamp(r))

    # Pro/Max accounts expose 5h/7d windows; Enterprise/overage use a single
    # spending-limit model reported via overage-utilization.
    if resp.headers.get("anthropic-ratelimit-unified-5h-utilization"):
        payload = {
            "s": pct(hdr("anthropic-ratelimit-unified-5h-utilization")),
            "sr": reset_minutes(hdr("anthropic-ratelimit-unified-5h-reset")),
            "srt": reset_at(hdr("anthropic-ratelimit-unified-5h-reset")),
            "w": pct(hdr("anthropic-ratelimit-unified-7d-utilization")),
            "wr": reset_minutes(hdr("anthropic-ratelimit-unified-7d-reset")),
            "wrt": reset_at(hdr("anthropic-ratelimit-unified-7d-reset")),
            "st": hdr("anthropic-ratelimit-unified-5h-status", "unknown"),
            "acct": "pro",
            "ok": True,
        }
    else:
        reset_ts = hdr("anthropic-ratelimit-unified-overage-reset")
        payload = {
            "s": pct(hdr("anthropic-ratelimit-unified-overage-utilization")),
            "sr": reset_minutes(reset_ts),
            "w": 0,
            "wr": 0,
            "st": hdr("anthropic-ratelimit-unified-status", "unknown"),
            "acct": "ent",
            **_billing_period_info(now, reset_ts),
            "ok": True,
        }
    return payload


def _billing_period_info(now: float, reset_ts: str) -> dict:
    """Fraction of billing period elapsed (tp, 0-100) and period length in days (pd).

    Billing periods are assumed calendar-monthly: period_end is the reset
    timestamp, period_start is the same day/time one calendar month earlier.

    The rate-limit headers expose only the reset timestamp, not the period
    length, so the monthly window is an assumption — but a documented one:
    Enterprise spend-limit `period` "the only value today is monthly"
    (Claude Enterprise Admin API reference). The doc notes period is an open
    string that may gain other values later; revisit this if so.
    """
    try:
        period_end = float(reset_ts)
    except ValueError:
        return {"tp": 0, "pd": 30}
    if period_end <= 0:
        # reset_ts defaults to "0" when the overage-reset header is absent.
        # fromtimestamp(0) is 1970; stepping a month back lands in 1969, and
        # datetime.timestamp() raises OSError for pre-1970 dates on Windows.
        # Benign on macOS/Linux, but guard here too to keep the daemons parallel.
        return {"tp": 0, "pd": 30}
    dt_end = datetime.datetime.fromtimestamp(period_end)
    prev_month = dt_end.month - 1 or 12
    prev_year = dt_end.year if dt_end.month > 1 else dt_end.year - 1
    prev_day = min(dt_end.day, calendar.monthrange(prev_year, prev_month)[1])
    dt_start = dt_end.replace(year=prev_year, month=prev_month, day=prev_day)
    period_start = dt_start.timestamp()
    period_len = period_end - period_start
    if period_len <= 0:
        return {"tp": 0, "pd": 30}
    pct_val = (now - period_start) / period_len * 100
    total_days = int(round(period_len / 86400))
    rd = f"{dt_end.strftime('%b')} {dt_end.day}"
    return {
        "tp": max(0, min(100, int(round(pct_val)))),
        "pd": total_days,
        "rd": rd,
    }


class PlanSelector:
    """Decide which config dir's plan is "active" across polls.

    "Active" = the plan whose session % rose most recently (recent API activity).
    A rise stamps a monotonic poll counter, so the choice is sticky and a window
    reset (a drop to 0) isn't mistaken for use. Before any rise is seen (startup)
    the highest current session % wins. Mirrors the Linux bash daemon.
    """

    def __init__(self) -> None:
        self.prev_s: dict[Path, int] = {}
        self.last_active: dict[Path, int] = {}
        self.seq = 0

    def choose(self, sessions: dict[Path, int]) -> Path:
        """Update state from this cycle's {dir: session_pct} and return the active dir."""
        self.seq += 1
        for d, s in sessions.items():
            if d in self.prev_s and s > self.prev_s[d]:
                self.last_active[d] = self.seq
            self.prev_s[d] = s
        # Most recent activity wins; ties (and the startup case) break by highest %.
        return max(sessions, key=lambda d: (self.last_active.get(d, 0), sessions[d]))


# Module-level so the active-plan state survives reconnects.
_SELECTOR = PlanSelector()



async def poll_active_payload(selector: PlanSelector = _SELECTOR) -> dict:
    """Poll every configured config dir and return the active plan's payload.

    Returns an {"ok": False, "err": ...} error beat when no dir yields a
    usable payload this cycle. A single configured dir (the default) collapses
    to exactly the old single-poll path.
    """
    dirs = read_config_dirs()
    payloads: dict[Path, dict] = {}
    sessions: dict[Path, int] = {}
    last_err: str | None = None
    for d in dirs:
        token = read_token_for(d)
        if not token:
            log(f"No token in {d}; skipping")
            continue
        result = await poll_api(token)
        if isinstance(result, str):
            last_err = result
            continue
        payloads[d] = result
        sessions[d] = int(result.get("s", 0) or 0)
    if not payloads:
        # Error beat: tells the firmware to flip to the idle view and show WHY
        # instead of rendering hours-old numbers as live ("token" = no config
        # dir had a readable token this cycle).
        return {"ok": False, "err": last_err or "token"}
    active = selector.choose(sessions)
    if len(dirs) > 1:
        log(f"Active plan: {active} (s={sessions[active]})")
    return payloads[active]


class Session:
    def __init__(self, client: BleakClient) -> None:
        self.client = client
        self.refresh_requested = asyncio.Event()

    def _on_refresh(self, _char, _data: bytearray) -> None:
        log("Refresh requested by device")
        self.refresh_requested.set()

    async def setup_refresh_subscription(self) -> None:
        # start_notify awaits CoreBluetooth's CCCD-write confirmation, which
        # never arrives if the peripheral doesn't ACK the subscribe (a
        # half-open link after the OS auto-connects the HID). Unbounded, that
        # await wedges the whole daemon between "Connected" and the first poll
        # — the device then shows nothing until a manual restart. Bound it: the
        # subscription is only an optional device-initiated refresh nudge (we
        # poll every POLL_INTERVAL regardless), so on timeout we proceed.
        try:
            await asyncio.wait_for(
                self.client.start_notify(REQ_CHAR_UUID, self._on_refresh),
                timeout=10,
            )
        except (BleakError, ValueError) as e:
            log(f"Refresh subscription unavailable: {e}")
        except asyncio.TimeoutError:
            log("Refresh subscription timed out; polling without it")

    async def write_payload(self, payload: dict) -> bool:
        # ensure_ascii=False: meeting titles are Cyrillic — raw UTF-8 is 2
        # bytes/char over the wire vs 6 for \uXXXX escapes (BLE_BUF_SIZE=512).
        data = json.dumps(payload, separators=(",", ":"),
                          ensure_ascii=False).encode()
        log(f"Sending: {data.decode()}")
        try:
            await self.client.write_gatt_char(RX_CHAR_UUID, data, response=False)
            return True
        except BleakError as e:
            log(f"Write failed: {e}")
            return False


def _is_encryption_error(exc: BaseException) -> bool:
    """True if a connect error is a macOS bonding/encryption mismatch.

    macOS reports a stale bond as CBErrorDomain Code=15 ("Failed to encrypt
    the connection..."). Match on the message text so we don't depend on how
    bleak wraps the underlying CoreBluetooth error.
    """
    s = str(exc).lower()
    return "code=15" in s or "encrypt" in s


# blueutil talks to Bluetooth via IOBluetooth, which on recent macOS needs its
# OWN Bluetooth TCC grant (separate from the daemon's CoreBluetooth grant).
# Without it, blueutil *hangs* instead of erroring — so every call is bounded
# by a timeout and a hang is reported as a permission problem, not a crash.
BLUEUTIL_TIMEOUT = 8


def _blueutil(*args: str) -> str | None:
    """Run `blueutil <args>`, returning stdout, or None on failure/timeout.

    A timeout almost always means blueutil lacks Bluetooth permission (it
    blocks rather than failing), so we surface that cause explicitly.
    """
    try:
        return subprocess.run(
            ["blueutil", *args],
            capture_output=True, text=True,
            timeout=BLUEUTIL_TIMEOUT, check=True,
        ).stdout
    except subprocess.TimeoutExpired:
        log(f"blueutil {' '.join(args)} timed out — it likely lacks Bluetooth "
            "permission. Grant it under System Settings > Privacy & Security > "
            "Bluetooth (run `blueutil --paired` once from Terminal to prompt).")
        return None
    except (subprocess.SubprocessError, OSError) as e:
        log(f"blueutil {' '.join(args)} failed: {e}")
        return None


def unpair_macos() -> bool:
    """Forget a stale macOS bond for DEVICE_NAME so the device can re-pair.

    A Code=15 "failed to encrypt" connect error means macOS holds bonding
    keys that no longer match the ESP32's (e.g. after a firmware reflash or
    the on-device bond-clear gesture). The firmware pairs "just works" (no
    MITM), so once the stale bond is gone the next connect re-bonds silently
    with no GUI prompt.

    CoreBluetooth exposes no unpair API, so we shell out to `blueutil`. The
    daemon only knows the peripheral's CoreBluetooth UUID, not the BD_ADDR
    that blueutil needs, so we map by name via `blueutil --paired`. Returns
    True if a bond was removed. Mirrors the Linux daemon's `bluetoothctl
    remove` self-heal.
    """
    if not shutil.which("blueutil"):
        log("Stale bond detected but `blueutil` is not installed; cannot "
            "auto-recover. Run `brew install blueutil`, or forget "
            f"'{DEVICE_NAME}' in System Settings > Bluetooth and reconnect.")
        return False

    out = _blueutil("--paired")
    if out is None:
        return False

    # Each line looks like:
    #   address: 28-84-85-55-5c-3d, ... name: "Clawdmeter", ...
    addr = None
    for line in out.splitlines():
        if f'name: "{DEVICE_NAME}"' in line:
            m = re.search(r"address:\s*([0-9a-fA-F:-]+)", line)
            if m:
                addr = m.group(1)
                break
    if not addr:
        log(f"No paired '{DEVICE_NAME}' found to unpair (already forgotten?)")
        return False

    if _blueutil("--unpair", addr) is None:
        return False
    log(f"Unpaired stale bond for '{DEVICE_NAME}' [{addr}]; re-pairing on "
        "next connect")
    return True


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
        # Bound the connect the same way #84 bounded the refresh subscribe.
        # On macOS the OS auto-connects the firmware's HID link, so
        # CoreBluetooth can hand us a half-open peripheral whose GATT connect
        # handshake never completes. BleakClient's own timeout governs
        # discovery, not connectPeripheral, so an unbounded await here wedges
        # the single-threaded daemon forever at "Connecting..." (observed ~13h,
        # device stuck on stale data). wait_for raises TimeoutError, which the
        # handler below already treats as a connection failure -> drop the
        # cached address and rescan.
        await asyncio.wait_for(client.connect(), timeout=CONNECT_TIMEOUT)
    except (BleakError, asyncio.TimeoutError) as e:
        log(f"Connection failed: {e}")
        if sys.platform == "darwin" and _is_encryption_error(e):
            log("Encryption failed — likely a stale macOS bond; self-healing")
            unpair_macos()
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
        last_payload: dict | None = None
        poll_interval = POLL_INTERVAL   # grows exponentially while polls fail
        while client.is_connected and not stop_event.is_set():
            now = time.time()
            elapsed = now - last_poll
            attn, attn_project = read_attention_flag()
            cal_url, cal_thresholds = read_cal_config()
            cal = None
            if not attn and cal_url:
                # Hook events outrank the calendar; an unsent reminder just
                # waits for the next TICK (nothing is marked sent yet).
                await refresh_cal_events(cal_url)
                cal = check_cal_reminder(cal_thresholds)
            if attn == "clear":
                # Dismiss-only: reuse the last payload, skip the API poll.
                if last_payload is not None:
                    log("Attention clear — dismissing the attention view")
                    a = await asyncio.to_thread(count_active_sessions)
                    await session.write_payload({**last_payload, "n": "clear",
                                                 "a": a})
            elif (session.refresh_requested.is_set() or elapsed >= poll_interval
                  or attn or cal):
                session.refresh_requested.clear()
                payload = await poll_active_payload()
                if payload.get("ok"):
                    last_payload = dict(payload)
                    poll_interval = POLL_INTERVAL
                else:
                    # Exponential backoff: a dead token means every retry is a
                    # guaranteed 401, and repeated auth failures escalate to
                    # 429s — don't hammer the API while there's nothing to win.
                    poll_interval = min(poll_interval * 2, 600)
                    log(f"Poll failed ({payload.get('err')}); next attempt in {poll_interval}s")
                # lsof inside can take a while — keep the BLE loop responsive.
                payload["a"] = await asyncio.to_thread(count_active_sessions)
                if attn:
                    # Attention events ride on error beats too — a permission
                    # chime matters even while the usage data is unavailable.
                    payload["n"] = attn
                    if attn_project:
                        payload["np"] = attn_project
                    log(f"Attention flag ({attn}, {attn_project or '?'}) — forwarding to device")
                elif cal:
                    payload["n"] = "cal"
                    payload["np"] = cal[0]
                    log(f"Calendar reminder — {cal[0]} ({cal[2]}')")
                if await session.write_payload(payload):
                    last_poll = time.time()
                    if cal and not attn:
                        cal_mark_sent(cal[1], cal[2], cal_thresholds)
                    if payload.get("ok"):
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
