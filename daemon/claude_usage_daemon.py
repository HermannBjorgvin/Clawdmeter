#!/usr/bin/env python3
"""Claude Usage Tracker Daemon (BLE) — macOS port of claude-usage-daemon.sh.

Polls Claude usage (the zero-token /usage endpoint, falling back to the
Messages API's rate-limit headers) and writes a JSON payload to the
ESP32 "Clawdmeter" peripheral over a custom GATT service. Uses
bleak (CoreBluetooth backend on macOS).

Polling and transmitting are separate: UsagePoller keeps a fixed schedule for
the daemon's whole life so the usage history stays complete while the hardware
is unplugged, and connect_and_run transmits whatever it has published.
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

try:
    from daemon.usage_history import UsageHistory
except ImportError:  # launched as a plain script (launchd), not as daemon.*
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    from daemon.usage_history import UsageHistory
from bleak import BleakClient
from bleak.exc import BleakError

DEVICE_NAME = "Clawdmeter"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"

POLL_INTERVAL = 60
TICK = 5
CONNECT_TIMEOUT = 20.0

# How stale a cached payload may be and still be worth handing to a device that
# has just connected. Polling runs on its own schedule (see UsagePoller), so the
# cache is normally well under one POLL_INTERVAL old; past two of them the last
# polls have been failing, and the device is better off holding its own idle
# screen than being shown numbers we already know are out of date.
PAYLOAD_MAX_AGE_S = POLL_INTERVAL * 2

# macOS: token lives in Keychain (service "Claude Code-credentials").
# Linux: token lives in ~/.claude/.credentials.json.
KEYCHAIN_SERVICE = "Claude Code-credentials"
DEFAULT_CONFIG_DIR = Path.home() / ".claude"
SAVED_ADDR_FILE = Path.home() / ".config" / "claude-usage-monitor" / "ble-address"
CONFIG_FILE = Path.home() / ".config" / "claude-usage-monitor" / "config"
HISTORY_STATE_FILE = Path.home() / ".config" / "claude-usage-monitor" / "history-state.json"

# BLE write sizing. The device's RX buffer is 512 bytes. A write-without-
# response is capped at ATT_MTU-3 and CoreBluetooth silently truncates past
# it; the base usage payload (~105 bytes) has always fit, but the History
# fields push a payload past what a conservative MTU allows. Anything over
# this threshold goes as a write-with-response, which CoreBluetooth chunks
# as a standard GATT long write and NimBLE reassembles on the device.
WRITE_NR_MAX_BYTES = 160
LONG_WRITE_TIMEOUT = 10.0

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

# --- Zero-token usage endpoint (primary source) -----------------------------
# The undocumented endpoint behind Claude Code's own `/usage` view. A GET
# costs nothing (no model call, no tokens billed) and returns the same 5h/7d
# windows the rate-limit headers carry, so it replaces the 1-token POST above
# as the primary source — API_URL stays as the fallback.
#
# UNDOCUMENTED means it can change shape or vanish without notice. Every
# unexpected response falls through to the header path rather than guessing,
# which is why the header path is kept intact rather than deleted.
USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
# Same auth/identity headers as the Messages call, minus Content-Type (no body
# on a GET). Derived from the template so the OAuth beta flag and User-Agent
# only ever have to be bumped in one place.
USAGE_HEADERS_TEMPLATE = {
    k: v for k, v in API_HEADERS_TEMPLATE.items() if k != "Content-Type"
}
# On a 429 the endpoint is benched for this long and the header path carries
# the daemon meanwhile. This is not a tuning knob: two earlier attempts at this
# feature (PRs #29/#37) were reverted upstream for hammering the endpoint, so
# a rate-limit response must cost us a long, quiet pause — never a retry.
USAGE_ENDPOINT_COOLDOWN_S = 900

# Monotonic-ish wall-clock deadline; 0.0 = not benched. Module-level so the
# bench survives reconnects and every config dir's poll, exactly like _SELECTOR
# — a 429 is an account-wide signal, not a per-dir one.
_usage_endpoint_benched_until: float = 0.0

# Severity values the endpoint reports, mapped onto the vocabulary the header
# path's "st" already uses ("allowed" / "allowed_warning" / "rejected"), so the
# key keeps one meaning across both paths. Unknown severities pass through
# verbatim rather than being flattened into a wrong-but-familiar word.
_SEVERITY_TO_STATUS = {
    "normal": "allowed",
    "warning": "allowed_warning",
    "exceeded": "rejected",
    "rejected": "rejected",
}


class TokenExpired(Exception):
    """Raised by poll_usage_endpoint/poll_api on a 401/403 — the access token is
    dead. The daemon never refreshes (pure free-ride: Claude Code owns
    refreshing), so the caller just signals "No data" to the device until the
    CLI re-seeds the token."""


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
        tok = data.get("accessToken")
        if isinstance(tok, str) and tok.strip():
            return tok
        # nested: {"claudeAiOauth": {"accessToken": "..."}}
        for v in data.values():
            if isinstance(v, dict):
                tok = v.get("accessToken")
                if isinstance(tok, str) and tok.strip():
                    return tok
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


def _read_token_keychain() -> str | None:
    """Read the OAuth access token from the macOS Keychain, or None.

    ``security … -w`` may hex-dump the stored secret (see _decode_keychain_blob),
    so decode before extracting the access token.
    """
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
    return _extract_access_token(_decode_keychain_blob(out.stdout))


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


def read_history_setting() -> str:
    """Read the `history` option from the config file. One of: on|off.

    Defaults to "on": the History screen is fed purely from local transcripts
    (no API call, no token), so there's nothing to opt into beyond the daemon
    itself. Set `history = off` to skip the transcript scan entirely.
    """
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip().lower() == "history":
                    val = val.strip().lower()
                    if val in ("off", "on"):
                        return val
    except OSError:
        pass
    return "on"


_HISTORY: UsageHistory | None = None


def _history() -> UsageHistory:
    """The process-wide history aggregator, built on first use from the
    configured config dirs and primed from its on-disk state."""
    global _HISTORY
    if _HISTORY is None:
        _HISTORY = UsageHistory(read_config_dirs(), state_file=HISTORY_STATE_FILE)
        _HISTORY.load()
    return _HISTORY


async def attach_history(payload: dict) -> None:
    """Merge the History fields (h/ht/hw/hm) into ``payload`` when enabled.

    The scan is incremental and normally milliseconds, but the very first one
    walks the whole retention window (a gigabyte-plus corpus is common), so it
    runs in a thread and never blocks the BLE loop. Any failure is logged and
    swallowed — the usage numbers must never depend on the transcript scan.
    """
    if read_history_setting() == "off":
        return
    try:
        hist = _history()
        t0 = time.time()
        await asyncio.to_thread(hist.scan)
        await asyncio.to_thread(hist.save)
        if hist.last_scan_files:
            log(f"History: read {hist.last_scan_bytes/1e6:.1f} MB from "
                f"{hist.last_scan_files} transcript(s) in {time.time()-t0:.2f}s")
        payload.update(hist.payload_fields())
        # "This week" on the device means Anthropic's rolling 7-day window,
        # which is what the weekly % meters — not the calendar week.
        start = hist.week_start_index(payload.get("wr"))
        if start is not None:
            payload["hs"] = start
        # Ground truth for the window grid: the API only ever reports the
        # *current* window's utilisation, so record it against the window it
        # belongs to while we can see it. Past windows fall back to an estimate.
        hist.observe(payload.get("s"), payload.get("sr"))
        # Which cell is the window currently burning — the device can't derive
        # it (an open window may have started yesterday).
        cell = hist.current_cell(payload.get("sr"))
        if cell is not None:
            payload["wc"] = cell
    except Exception as e:  # noqa: BLE001 — deliberately broad, see docstring
        log(f"History scan failed (continuing without it): {e}")


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


def _minutes_until(iso_ts: object, now: float) -> int:
    """Whole minutes from ``now`` until an ISO-8601 instant; 0 when already past.

    The endpoint reports a real timestamp ("2026-09-06T10:19:59.672495+00:00")
    where the headers only carried a whole-minute epoch, so this is strictly
    more precise — but it rounds to the same unit the firmware expects, and
    matches the header path's ``int(round(mins))`` so "sr"/"wr" keep one
    meaning across both sources.

    A clock skew or a window that expired mid-poll must never produce a
    negative: the firmware renders "sr" as a countdown and a negative would
    read as a garbage reset time.
    """
    if not isinstance(iso_ts, str):
        # null resets_at (seen on the inactive buckets) — same as an absent
        # header on the old path: no countdown to show.
        return 0
    try:
        dt = datetime.datetime.fromisoformat(iso_ts)
    except ValueError:
        return 0
    if dt.tzinfo is None:
        # Every observed value carries an explicit offset. If one ever doesn't,
        # UTC is the right assumption for an API instant — .timestamp() would
        # otherwise silently read it as host-local time and skew by the tz.
        dt = dt.replace(tzinfo=datetime.timezone.utc)
    mins = (dt.timestamp() - now) / 60.0
    return int(round(mins)) if mins > 0 else 0


def _endpoint_pct(util: object) -> int:
    """Round the endpoint's ``utilization`` to whole percent.

    NOTE the units difference that makes this NOT the header path's ``pct()``:
    the endpoint already reports a percentage (16.0 means 16%), whereas the
    rate-limit headers report a 0..1 fraction that has to be multiplied by 100.
    Multiplying here would send 1600% to the device.
    """
    if isinstance(util, bool) or not isinstance(util, (int, float)):
        return 0
    return max(0, int(round(float(util))))


def _is_pro_max_usage(data: object) -> bool:
    """Does this response look like the Pro/Max shape we know how to read?

    Pro/Max accounts report both rolling windows as objects with a numeric
    ``utilization``. Enterprise/overage accounts are a different model
    entirely (a single spend limit, surfaced to us only as the
    ``anthropic-ratelimit-unified-overage-*`` headers) and are the reason
    ``poll_api`` also produces the "ent" acct value and _billing_period_info().
    Nothing here can synthesise those, so anything that isn't unmistakably
    Pro/Max — an Enterprise account, a plan we've never seen, or a future
    reshaping of this undocumented payload — falls back to the headers rather
    than being guessed at.
    """
    if not isinstance(data, dict):
        return False
    for key in ("five_hour", "seven_day"):
        window = data.get(key)
        if not isinstance(window, dict):
            return False
        util = window.get("utilization")
        # bool is an int subclass; True must not read as 1%.
        if isinstance(util, bool) or not isinstance(util, (int, float)):
            return False
    return True


def _endpoint_status(data: dict) -> str:
    """The "st" string, from the ``limits`` entry describing the 5h window.

    The headers expose a dedicated 5h status; the endpoint instead carries a
    per-limit ``severity``, so the session-kind limit is the equivalent. A
    payload without a usable ``limits`` list still yields a valid payload —
    the firmware parses "st" but renders nothing from it, so "unknown" (the
    header path's own default) is a fine answer.
    """
    limits = data.get("limits")
    if not isinstance(limits, list):
        return "unknown"
    for entry in limits:
        if isinstance(entry, dict) and entry.get("kind") == "session":
            severity = entry.get("severity")
            if isinstance(severity, str):
                return _SEVERITY_TO_STATUS.get(severity, severity)
    return "unknown"


async def poll_usage_endpoint(token: str) -> dict | None:
    """Primary usage source: the zero-token GET behind Claude Code's /usage.

    Returns a payload dict in exactly the shape ``poll_api`` produces (same
    keys, same units, chime/clock fields already merged) so callers can use it
    as a drop-in, or ``None`` meaning "I have nothing usable — use the header
    path". Only a dead token escapes as an exception.

    Everything unexpected returns None rather than raising: a non-200, a
    network error, a body that isn't JSON, or a body whose shape we don't
    recognise. The endpoint is undocumented, so that fallback IS the safety
    story — this function is allowed to be wrong, never to be load-bearing.

    Deliberately NOT surfaced: the per-model weekly bucket (``limits`` entries
    with kind == "weekly_scoped"). Anthropic removes Fable's separate weekly
    limit on 2026-09-14, so the extra keys would be dead within days, and the
    BLE payload is already large enough that the History fields push it past
    a write-without-response (see WRITE_NR_MAX_BYTES). Nothing should depend
    on that bucket; if it's ever wanted, it belongs behind new optional keys.
    """
    global _usage_endpoint_benched_until
    if time.time() < _usage_endpoint_benched_until:
        # Benched by a recent 429; stay off it entirely and let the caller
        # fall through. No log line — this fires every poll for 15 minutes.
        return None

    headers = dict(USAGE_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {token}"
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.get(USAGE_URL, headers=headers)
    except httpx.HTTPError as e:
        log(f"Usage endpoint call failed ({e}); using rate-limit headers")
        return None

    # 401/403 is a dead token, not an endpoint problem — the header path would
    # only spend a request to reach the same conclusion. Raise exactly as
    # poll_api does; the caller reads it as "this config dir has no data".
    if resp.status_code in (401, 403):
        log(f"Usage endpoint HTTP {resp.status_code} (token expired/invalid)")
        raise TokenExpired()
    if resp.status_code == 429:
        _usage_endpoint_benched_until = time.time() + USAGE_ENDPOINT_COOLDOWN_S
        log(f"Usage endpoint rate-limited; benched for "
            f"{USAGE_ENDPOINT_COOLDOWN_S // 60} min, using rate-limit headers")
        return None
    if resp.status_code != 200:
        log(f"Usage endpoint HTTP {resp.status_code}; using rate-limit headers")
        return None

    try:
        data = resp.json()
    except ValueError as e:  # json.JSONDecodeError subclasses ValueError
        log(f"Usage endpoint returned non-JSON ({e}); using rate-limit headers")
        return None

    if not _is_pro_max_usage(data):
        log("Usage endpoint shape not recognised; using rate-limit headers")
        return None

    # Re-read the clock after the request: the round trip is part of the
    # elapsed time the countdown is measured from.
    now = time.time()
    five_hour = data["five_hour"]
    seven_day = data["seven_day"]
    payload = {
        "s": _endpoint_pct(five_hour.get("utilization")),
        "sr": _minutes_until(five_hour.get("resets_at"), now),
        "w": _endpoint_pct(seven_day.get("utilization")),
        "wr": _minutes_until(seven_day.get("resets_at"), now),
        "st": _endpoint_status(data),
        "acct": "pro",
        "ok": True,
    }
    add_chime_field(payload)   # adds "c":1 iff the config opts in
    add_clock_fields(payload)   # adds "t" + "tf" iff the config opts in
    return payload


async def poll_api(token: str) -> dict | None:
    # Primary path: the zero-token usage endpoint. It returns None for anything
    # it can't confidently read (non-200, network error, bad/unknown JSON, a
    # 429 bench) and the rate-limit header path below then runs unchanged.
    #
    # The composition lives here rather than in poll_active so poll_api stays
    # the daemon's single "give me a payload for this token" chokepoint — every
    # caller and every test that stubs poll_api keeps working, and there is
    # exactly one place where a network call can escape.
    payload = await poll_usage_endpoint(token)   # may raise TokenExpired
    if payload is not None:
        return payload

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

    # Pro/Max accounts expose 5h/7d windows; Enterprise/overage use a single
    # spending-limit model reported via overage-utilization.
    if resp.headers.get("anthropic-ratelimit-unified-5h-utilization"):
        payload = {
            "s": pct(hdr("anthropic-ratelimit-unified-5h-utilization")),
            "sr": reset_minutes(hdr("anthropic-ratelimit-unified-5h-reset")),
            "w": pct(hdr("anthropic-ratelimit-unified-7d-utilization")),
            "wr": reset_minutes(hdr("anthropic-ratelimit-unified-7d-reset")),
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
    add_chime_field(payload)   # adds "c":1 iff the config opts in
    add_clock_fields(payload)   # adds "t" + "tf" iff the config opts in
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


async def poll_active(selector: PlanSelector = _SELECTOR) -> tuple[dict | None, bool]:
    """Poll every configured config dir; return ``(active_payload, all_dead)``.

    ``active_payload`` — the active plan's payload dict, or None when no dir
    yields a usable payload this cycle. A single configured dir (the default)
    collapses to exactly the old single-poll path.

    ``all_dead`` — True when *every* configured dir lacked a usable token this
    cycle (file/Keychain empty, or a 401/expired token), so the caller can
    signal "No data". False when at least one token authenticated — including a
    transient non-auth poll failure worth retrying silently rather than idling.

    Pure free-ride: a 401 (TokenExpired) means that dir's token has expired and
    only Claude Code (its owner) can re-seed it — we never refresh it ourselves.
    """
    dirs = read_config_dirs()
    payloads: dict[Path, dict] = {}
    sessions: dict[Path, int] = {}
    any_live = False
    for d in dirs:
        token = read_token_for(d)
        if not token:
            log(f"No token in {d}; skipping")
            continue
        try:
            payload = await poll_api(token)
        except TokenExpired:
            log(f"Token in {d} expired/invalid; skipping")
            continue
        # Authenticated: a transient None here isn't an auth failure, so the
        # dir counts as live and we stay silent rather than idling the device.
        any_live = True
        if payload is not None:
            payloads[d] = payload
            sessions[d] = int(payload.get("s", 0) or 0)
    if not payloads:
        return None, not any_live
    active = selector.choose(sessions)
    if len(dirs) > 1:
        log(f"Active plan: {active} (s={sessions[active]})")
    return payloads[active], False


async def poll_active_payload(selector: PlanSelector = _SELECTOR) -> dict | None:
    """The active plan's payload, or None when no dir yields one this cycle.

    Thin wrapper over :func:`poll_active` for callers that don't need the
    all-dead flag.
    """
    payload, _dead = await poll_active(selector)
    return payload


async def _wait_first(*events: asyncio.Event, timeout: float) -> None:
    """Return when any of ``events`` is set, or after ``timeout`` seconds.

    Lets a wait wake immediately on a stop signal (prompt SIGINT/SIGTERM exit)
    without losing the other wakeups it is there for. Cancels and drains the
    loser tasks so they don't warn. Mirrors the Windows daemon's helper of the
    same name — the two daemons keep the same shutdown behaviour.
    """
    tasks = [asyncio.ensure_future(e.wait()) for e in events]
    try:
        await asyncio.wait(tasks, timeout=timeout, return_when=asyncio.FIRST_COMPLETED)
    finally:
        for t in tasks:
            t.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)


class UsagePoller:
    """Owns the usage-poll schedule, independent of the BLE link.

    WHY this is not driven from connect_and_run any more: the poll results are
    the ground truth behind ``usage_history.observe()`` — each 5-hour window's
    measured peak, and the tokens-per-percent ratio calibrated from those peaks.
    Polling only while a device happened to be connected meant unplugging the
    hardware and carrying on working left holes in the history, not just in the
    live display. Polling was gated on a connection because it used to cost a
    token per call (a POST to /v1/messages), so polling with nothing to display
    on was pure waste; the primary path is now the zero-token GET on USAGE_URL,
    so the reason for the gating is gone.

    One task does all the polling for the daemon's lifetime. It publishes "the
    payload a device should be shown" — a usage payload or the {"ok": False}
    no-data beat, History fields already merged — and connect_and_run only
    transmits what is already published. A device never has to be connected,
    or ever to have been seen, for a cycle to run.

    Because exactly one task polls, a device-requested refresh can never run
    concurrently with a scheduled poll: request_poll() just nudges this loop's
    sleep. The nudge flag is cleared *after* a cycle completes, so a request
    that arrives while a poll is already in flight is answered by that poll's
    result instead of queueing a second poll immediately behind it.
    """

    def __init__(
        self,
        stop_event: asyncio.Event,
        selector: PlanSelector = _SELECTOR,
    ) -> None:
        self.stop_event = stop_event
        self.selector = selector
        # Latest payload to send, or None until the first cycle produces one.
        self.payload: dict | None = None
        self.published_at: float = 0.0
        # Bumped on every publish. Consumers compare it against the last value
        # they sent, so a reconnect re-sends the cache and a failed write is
        # retried next tick without costing another poll.
        self.seq: int = 0
        self._wake = asyncio.Event()
        # The "no usable token" notice used to be bounded by a device being
        # connected; now it would repeat every POLL_INTERVAL for as long as the
        # user stays logged out, so log it on the transition only.
        self._warned_no_token = False

    def request_poll(self) -> None:
        """Ask for an early poll — the firmware's refresh nudge."""
        self._wake.set()

    def fresh_payload(self, now: float | None = None) -> tuple[dict | None, int]:
        """The cached payload and its sequence number, or (None, seq) when the
        cache is too old to be worth sending (see PAYLOAD_MAX_AGE_S)."""
        if self.payload is None:
            return None, self.seq
        age = (now if now is not None else time.time()) - self.published_at
        if age > PAYLOAD_MAX_AGE_S:
            return None, self.seq
        return self.payload, self.seq

    def _publish(self, payload: dict) -> None:
        self.payload = payload
        self.published_at = time.time()
        self.seq += 1

    async def poll_once(self) -> None:
        """One cycle: read the token(s), poll, attach history, publish.

        Pure free-ride: read whatever access token(s) Claude Code currently
        holds across the configured config dirs and NEVER refresh them
        ourselves. Claude Code (the token's owner) does all refreshing;
        refreshing here would race its rotation and feed the OAuth endpoint's
        rate limit (429). When no dir has a usable token we publish "No data"
        so a connected device idles instead of holding stale numbers until the
        CLI re-seeds it.
        """
        payload, dead = await poll_active(self.selector)
        if payload is not None:
            # attach_history() runs on the POLL schedule, not the send
            # schedule: it is what calls observe(), which records the live
            # window's peak and keeps the tokens-per-percent calibration
            # learning. Skipping it while no device is connected would leave
            # exactly the gaps this class exists to close.
            await attach_history(payload)
            self._publish(payload)
            self._warned_no_token = False
        elif dead:
            # No live token in any config dir (missing, or a 401/expired token)
            # -> publish "No data" so a connected device shows its idle screen
            # rather than stale numbers.
            if not self._warned_no_token:
                log("No usable token; signalling no-data to device — run "
                    "`claude login` or use the CLI to let Claude Code renew it")
                self._warned_no_token = True
            beat: dict = {"ok": False}
            await attach_history(beat)   # local history needs no token
            self._publish(beat)
        else:
            # Transient poll failure (a live token that didn't answer this
            # cycle) -> publish nothing, so a connected device stays on its
            # last payload and we retry on the next scheduled poll.
            log("No usable config dir this cycle")

    async def run(self) -> None:
        """The poll loop. Runs until stop_event is set (or the task is
        cancelled); one poll happens immediately at startup so the daemon has
        data to hand out before the first device ever connects."""
        while not self.stop_event.is_set():
            try:
                await self.poll_once()
            except asyncio.CancelledError:
                raise
            except Exception as e:  # noqa: BLE001 — the schedule must outlive
                # any single bad cycle; dropping the task would silently stop
                # the history calibration for the rest of the daemon's life.
                log(f"Poll cycle failed (continuing): {e}")
            # Cleared here, after the cycle, so an in-flight poll absorbs any
            # refresh request that arrived while it was running.
            self._wake.clear()
            await _wait_first(self.stop_event, self._wake, timeout=POLL_INTERVAL)


def _with_current_clock(payload: dict) -> dict:
    """``payload`` with its wall-clock fields re-stamped for now, if it has any.

    The firmware anchors its clock to the moment a payload lands (clock_base_ms
    in ui.cpp), so sending a cached payload verbatim would set the device's
    clock back by the payload's own age — up to a POLL_INTERVAL when a device
    connects mid-cycle. Only "t"/"tf" are time-sensitive; the usage numbers are
    by definition as fresh as the last poll. Returns the original object
    untouched when the clock is switched off, so the common path allocates
    nothing.
    """
    if "t" not in payload:
        return payload
    stamped = dict(payload)
    add_clock_fields(stamped)
    return stamped


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
        data = json.dumps(payload, separators=(",", ":")).encode()
        log(f"Sending: {data.decode()}")
        # Small payloads keep the historical write-without-response path
        # exactly as it was. Larger ones (History fields) would be silently
        # truncated by CoreBluetooth at ATT_MTU-3, so they go with-response —
        # a GATT long write — bounded like the subscribe, since an ACK that
        # never comes on a half-open link must not wedge the poll loop.
        long_write = len(data) > WRITE_NR_MAX_BYTES
        try:
            if long_write:
                await asyncio.wait_for(
                    self.client.write_gatt_char(RX_CHAR_UUID, data, response=True),
                    timeout=LONG_WRITE_TIMEOUT,
                )
            else:
                await self.client.write_gatt_char(RX_CHAR_UUID, data, response=False)
            return True
        except BleakError as e:
            log(f"Write failed: {e}")
            return False
        except asyncio.TimeoutError:
            log(f"Write timed out ({len(data)} bytes, with-response)")
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


async def connect_and_run(
    target, stop_event: asyncio.Event, poller: UsagePoller
) -> bool:
    """Connect to a target and transmit until disconnected or stopped.

    ``target`` is either an address string (Linux) or a BLEDevice carrying
    live CoreBluetooth details (macOS). Returns True if the connection was
    used successfully (so the caller keeps the cached address), False if the
    connection failed and the cache should be invalidated.

    This function no longer polls: ``poller`` owns the schedule and runs whether
    or not anything is connected (see UsagePoller). All that happens here is
    transmitting each newly published payload, plus an immediate send of the
    cache on connect so a device doesn't wait out a whole POLL_INTERVAL for its
    first screenful.
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

    # 0 = nothing sent on THIS connection yet, and the poller's seq starts at 0
    # and only ever rises — so a cache published before this connect is sent
    # straight away, and a reconnect re-sends it rather than idling the device.
    last_sent_seq = 0
    used_successfully = False
    try:
        while client.is_connected and not stop_event.is_set():
            resend = False
            if session.refresh_requested.is_set():
                session.refresh_requested.clear()
                # The firmware nudges when it has nothing to show, so answer
                # from the cache immediately, and ask the poller for a fresh
                # reading to follow. request_poll() can't start a second
                # concurrent poll — see UsagePoller.
                resend = True
                poller.request_poll()

            payload, seq = poller.fresh_payload()
            if payload is not None and (resend or seq != last_sent_seq):
                if await session.write_payload(_with_current_clock(payload)):
                    last_sent_seq = seq
                    used_successfully = True
                # A failed write leaves last_sent_seq alone, so the same
                # payload is retried on the next tick — no extra poll, and no
                # POLL_INTERVAL of silence on what may be a healthy link.

            # Wake on a refresh request, on stop (prompt SIGINT/SIGTERM exit),
            # or every TICK — the periodic wake is what notices a disconnect
            # quickly and picks up the poller's newly published payloads.
            await _wait_first(session.refresh_requested, stop_event, timeout=TICK)
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

    # Polling starts here and runs for the daemon's whole life, on its own task:
    # usage is recorded whether or not a device is connected, and whether or not
    # one has ever been seen. The BLE loop below only transmits what the poller
    # has already published.
    poller = UsagePoller(stop_event)
    poll_task = asyncio.create_task(poller.run())

    backoff = 1
    skip_addr: str | None = None  # macOS: a peripheral to skip for one cycle
    try:
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
            ok = await connect_and_run(target, stop_event, poller)
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
    finally:
        # The poll loop returns on its own once stop_event is set; the cancel
        # covers a poll (or its sleep) already in flight, and unwinding for any
        # other reason. Awaiting it means no task outlives asyncio.run().
        stop_event.set()
        poll_task.cancel()
        await asyncio.gather(poll_task, return_exceptions=True)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
