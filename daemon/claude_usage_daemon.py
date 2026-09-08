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
from datetime import datetime, timezone
from pathlib import Path

import httpx
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

# Windows: prevent gh/git subprocess calls from flashing a console window
# when the daemon itself runs without one (e.g. under the Scheduled Task).
_NO_WINDOW = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0

DEVICE_NAME = "Claude Controller"
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID = "4c41555a-4465-7669-6365-000000000004"

POLL_INTERVAL = 60
COPILOT_POLL_INTERVAL = 300  # 5 minutes
SYSINFO_POLL_INTERVAL = 30   # 30 seconds
VSCODE_POLL_INTERVAL = 30    # 30 seconds
ENV_POLL_INTERVAL = 900      # 15 minutes (clock + weather)
ACT_POLL_INTERVAL = 5        # Claude activity — needs to feel responsive
CI_POLL_INTERVAL = 120       # 2 minutes (CI status + review queue + git)
SUM_POLL_INTERVAL = 300      # 5 minutes (daily summary)
TICK = 5

# Approx first-party token rates, $/1M (input, output). Cache reads ~= 0.1x in.
MODEL_RATES = {
    "opus": (5.0, 25.0), "sonnet": (3.0, 15.0), "haiku": (1.0, 5.0),
    "fable": (10.0, 50.0),
}
CLAUDE_CTX_WINDOW = 200_000  # Claude Code default working window
SCAN_TIMEOUT = 8.0

# macOS: token lives in Keychain (service "Claude Code-credentials").
# Linux: token lives in ~/.claude/.credentials.json.
KEYCHAIN_SERVICE = "Claude Code-credentials"
DEFAULT_CONFIG_DIR = Path.home() / ".claude"
CREDENTIALS_PATH = DEFAULT_CONFIG_DIR / ".credentials.json"
CONFIG_FILE = Path.home() / ".config" / "claude-usage-monitor" / "config"
SAVED_ADDR_FILE = Path.home() / ".config" / "claude-usage-monitor" / "ble-address"
LAST_PAYLOAD_FILE = Path.home() / ".config" / "claude-usage-monitor" / "last-payloads.json"
GEO_CACHE_FILE = Path.home() / ".config" / "claude-usage-monitor" / "location.json"
ACTIVITY_LOG = Path.home() / ".config" / "claude-usage-monitor" / "activity.log"
CLAUDE_PROJECTS_DIR = Path.home() / ".claude" / "projects"
TOKEN_ENV_VARS = (
    "CLAUDE_CODE_OAUTH_TOKEN",
    "ANTHROPIC_AUTH_TOKEN",
    "CLAUDE_ACCESS_TOKEN",
    "ANTHROPIC_API_KEY",
)

_missing_token_logged = False

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

# Claude Code OAuth — used to refresh an expired access token ourselves rather
# than waiting for `claude` to run. Public client id; UA must NOT be claude-code.
OAUTH_CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e"
OAUTH_TOKEN_URLS = (
    "https://console.anthropic.com/v1/oauth/token",
    "https://platform.claude.com/v1/oauth/token",
)
_LAST_API_STATUS = 0   # HTTP status of the most recent poll_api() call


_LOG_FILE: Path | None = None


def log(msg: str) -> None:
    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
    print(line, flush=True)
    if _LOG_FILE is not None:
        try:
            if _LOG_FILE.exists() and _LOG_FILE.stat().st_size > 1_000_000:
                tail = _LOG_FILE.read_text(encoding="utf-8", errors="ignore").splitlines()[-2000:]
                _LOG_FILE.write_text("\n".join(tail) + "\n", encoding="utf-8")
            with open(_LOG_FILE, "a", encoding="utf-8") as f:
                f.write(f"{time.strftime('%Y-%m-%d')} {line}\n")
        except OSError:
            pass


def read_github_token() -> str | None:
    """Get GitHub auth token via `gh auth token` CLI."""
    try:
        result = subprocess.run(
            ["gh", "auth", "token"],
            capture_output=True, text=True, timeout=10, creationflags=_NO_WINDOW,
        )
        if result.returncode == 0:
            token = result.stdout.strip()
            if token:
                return token
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass
    return None


async def poll_copilot(gh_token: str) -> dict | None:
    """Poll /copilot_internal/user for premium request quota.

    Returns quota_snapshots.premium_interactions (percent_remaining, remaining,
    entitlement) and quota_reset_date_utc. Works for individual Copilot Pro plan.
    """
    headers = {
        "Authorization": f"token {gh_token}",
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
    }
    result: dict = {
        "src": "copilot",
        "en": False,
        "plan": "unknown",
        "pp": -1,   # premium_pct_used (0-100)
        "pr": -1,   # premium_remaining
        "pe": -1,   # premium_entitlement (total)
        "prm": -1,  # minutes until quota reset
        "ok": True,
    }
    try:
        async with httpx.AsyncClient(timeout=15.0) as http:
            resp = await http.get(
                "https://api.github.com/copilot_internal/user", headers=headers
            )
            if resp.status_code == 200:
                data = resp.json()
                result["en"] = True
                plan_raw = data.get("copilot_plan", "individual").lower()
                _plan_map = {
                    "individual": "Pro", "pro": "Pro", "copilot_pro": "Pro",
                    "business": "Business", "enterprise": "Enterprise",
                }
                result["plan"] = _plan_map.get(plan_raw, plan_raw.title())

                # premium_interactions quota
                pi = (data.get("quota_snapshots") or {}).get("premium_interactions") or {}
                pct_rem = pi.get("percent_remaining", -1)
                if pct_rem >= 0:
                    result["pp"] = int(round(100 - pct_rem))
                remaining = pi.get("remaining", -1)
                if isinstance(remaining, (int, float)) and remaining >= 0:
                    result["pr"] = int(remaining)
                entitlement = pi.get("entitlement", -1)
                if isinstance(entitlement, (int, float)) and entitlement > 0:
                    result["pe"] = int(entitlement)

                # Minutes until monthly reset
                reset_date = data.get("quota_reset_date_utc") or data.get("quota_reset_date")
                if reset_date:
                    try:
                        if "T" in reset_date:
                            reset_dt = datetime.fromisoformat(reset_date.replace("Z", "+00:00"))
                        else:
                            reset_dt = datetime.fromisoformat(reset_date + "T00:00:00+00:00")
                        mins = int((reset_dt - datetime.now(timezone.utc)).total_seconds() / 60)
                        result["prm"] = max(0, mins)
                        local_dt = reset_dt.astimezone()
                        result["prd"] = local_dt.strftime("%b ") + str(local_dt.day)
                    except Exception:
                        pass
            elif resp.status_code == 404:
                log("Copilot: /copilot_internal/user returned 404")
    except httpx.HTTPError as e:
        log(f"Copilot API error: {e}")
        result["ok"] = False
    log(
        f"Copilot: plan={result['plan']} enabled={result['en']} "
        f"premium={result['pp']}% remaining={result['pr']}/{result['pe']} reset_mins={result['prm']}"
    )
    return result


def poll_sysinfo() -> dict:
    """Collect CPU, RAM, and disk stats via psutil.

    Returns a dict ready to send as a BLE JSON payload with src='sysinfo'.
    All fields fall back to -1 / 0.0 when unavailable (e.g. no sensor data on macOS).
    """
    result: dict = {
        "src": "sysinfo",
        "cpu": -1,   # CPU utilization 0-100
        "ct": -1.0,  # CPU temperature °C
        "rp": -1,    # RAM used %
        "ru": 0.0,   # RAM used GB
        "rt": 0.0,   # RAM total GB
        "dp": -1,    # Disk used %
        "du": 0.0,   # Disk used GB
        "dt": 0.0,   # Disk total GB
    }
    try:
        import psutil  # type: ignore[import-untyped]

        result["cpu"] = int(psutil.cpu_percent(interval=None))

        try:
            temps = psutil.sensors_temperatures()
            if temps:
                # Prefer coretemp / k10temp / cpu_thermal; fall back to first entry
                for key in ("coretemp", "k10temp", "cpu_thermal", "acpitz"):
                    if key in temps and temps[key]:
                        result["ct"] = round(temps[key][0].current, 1)
                        break
                else:
                    first_group = next(iter(temps.values()))
                    if first_group:
                        result["ct"] = round(first_group[0].current, 1)
        except (AttributeError, OSError):
            pass  # macOS: sensors_temperatures() not available

        vm = psutil.virtual_memory()
        result["rp"] = int(vm.percent)
        result["ru"] = round(vm.used / 1e9, 1)
        result["rt"] = round(vm.total / 1e9, 1)

        disk_path = (os.getenv('SystemDrive', 'C:') + os.sep) if sys.platform.startswith('win') else '/'
        du = psutil.disk_usage(disk_path)
        result["dp"] = int(du.percent)
        result["du"] = round(du.used / 1e9, 0)
        result["dt"] = round(du.total / 1e9, 0)

        log(
            f"Sysinfo: cpu={result['cpu']}% temp={result['ct']}°C "
            f"ram={result['rp']}% ({result['ru']}/{result['rt']} GB) "
            f"disk={result['dp']}% ({result['du']}/{result['dt']} GB)"
        )
    except ImportError:
        if not getattr(poll_sysinfo, "_missing_logged", False):
            log("psutil not installed; skipping sysinfo poll (pip install psutil)")
            setattr(poll_sysinfo, "_missing_logged", True)
    except Exception as exc:
        log(f"Sysinfo poll error: {exc}")

    return result


def poll_vscode() -> dict:
    """Collect VS Code process stats and recent log errors.

    Aggregates all Code/code processes for memory and CPU, counts extension
    hosts, and scans VS Code log files for errors/criticals in the last 30 min.
    Returns a dict with src='vscode' ready to send as a BLE JSON payload.
    """
    import re as _re

    result: dict = {
        "src": "vscode",
        "mm": -1,   # total RSS in MB
        "vc": -1,   # total CPU %
        "xe": -1,   # extension host process count
        "ec": -1,   # error count (last 30 min)
        "le": "",   # last error snippet
    }

    # --- Process stats via psutil ---
    try:
        import psutil  # type: ignore[import-untyped]

        vscode_procs = []
        ext_hosts = 0
        for proc in psutil.process_iter(["name", "cmdline", "memory_info", "cpu_percent", "status"]):
            try:
                name = (proc.info["name"] or "").lower()
                cmdline = " ".join(proc.info["cmdline"] or []).lower()
                if name in ("code.exe", "code", "code-insiders", "code-insiders.exe") or \
                        "visual studio code" in cmdline or \
                        ("/code/" in cmdline and "electron" in cmdline):
                    vscode_procs.append(proc)
                    if "extensionhost" in name or "extensionhost" in cmdline:
                        ext_hosts += 1
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                continue

        if vscode_procs:
            total_rss = sum(
                p.info["memory_info"].rss for p in vscode_procs
                if p.info.get("memory_info")
            )
            total_cpu = sum(
                p.info["cpu_percent"] or 0 for p in vscode_procs
            )
            result["mm"] = int(total_rss / 1e6)
            result["vc"] = int(round(total_cpu))
            result["xe"] = ext_hosts

    except ImportError:
        log("psutil not installed; skipping vscode poll (pip install psutil)")
    except Exception as exc:
        log(f"VS Code process poll error: {exc}")

    # --- Log scan for errors in last 30 min ---
    try:
        log_roots = []
        if sys.platform == "win32":
            appdata = os.environ.get("APPDATA", "")
            if appdata:
                log_roots.append(Path(appdata) / "Code" / "logs")
                log_roots.append(Path(appdata) / "Code - Insiders" / "logs")
        elif sys.platform == "darwin":
            log_roots.append(Path.home() / "Library" / "Application Support" / "Code" / "logs")
        else:
            log_roots.append(Path.home() / ".config" / "Code" / "logs")

        cutoff = time.time() - 1800  # 30 minutes
        error_pat = _re.compile(r"\[error\]|\[critical\]", _re.IGNORECASE)
        error_count = 0
        last_error = ""

        for log_root in log_roots:
            if not log_root.exists():
                continue
            # Find all .log files modified in the last 30 min
            for log_file in sorted(log_root.rglob("*.log"), key=lambda p: p.stat().st_mtime, reverse=True):
                try:
                    if log_file.stat().st_mtime < cutoff:
                        continue
                    with open(log_file, "r", encoding="utf-8", errors="ignore") as f:
                        for line in f:
                            if error_pat.search(line):
                                error_count += 1
                                # Keep last error as a short snippet
                                snippet = line.strip()
                                # Strip timestamp prefix like "[2024-01-01 12:00:00.000] "
                                snippet = _re.sub(r"^\[[\d\-T :\.Z]+\]\s*", "", snippet)
                                snippet = _re.sub(r"\[(?:error|critical)\]\s*", "", snippet, flags=_re.IGNORECASE)
                                last_error = snippet[:28]
                except (OSError, PermissionError):
                    continue

        result["ec"] = error_count
        result["le"] = last_error

    except Exception as exc:
        log(f"VS Code log scan error: {exc}")
        result["ec"] = 0

    log(
        f"VS Code: mem={result['mm']}MB cpu={result['vc']}% "
        f"ext_hosts={result['xe']} errors={result['ec']}"
    )
    return result


# Fallback when the config names no location and none is cached.
DEFAULT_LOCATION = (45.7489, 21.2087, "Timisoara")  # Timisoara, Romania


def _ascii(s: str) -> str:
    """The device fonts are ASCII-only — fold diacritics away before sending."""
    return s.encode("ascii", "ignore").decode()


async def _geocode(name: str) -> tuple[float, float, str] | None:
    try:
        async with httpx.AsyncClient(timeout=10.0) as http:
            resp = await http.get(
                "https://geocoding-api.open-meteo.com/v1/search",
                params={"name": name, "count": 1},
            )
        results = (resp.json() or {}).get("results") or []
        if results:
            r = results[0]
            return (float(r["latitude"]), float(r["longitude"]), r.get("name") or name)
    except (httpx.HTTPError, KeyError, ValueError) as e:
        log(f"Geocoding '{name}' failed: {e}")
    return None


async def _resolve_location() -> tuple[float, float, str]:
    """(lat, lon, label). Configurable in ~/.config/claude-usage-monitor/config:

        location = Berlin          # city name, geocoded (result cached)
        lat = 48.85 / lon = 2.35   # or explicit coordinates (win over `location`)

    Defaults to Timisoara, Romania.
    """
    cfg = read_config()

    try:
        if cfg.get("lat") and cfg.get("lon"):
            return (float(cfg["lat"]), float(cfg["lon"]),
                    cfg.get("location") or "Home")
    except (TypeError, ValueError):
        log("Config lat/lon malformed; ignoring")

    name = cfg.get("location", "").strip()
    if not name:
        return DEFAULT_LOCATION

    try:
        c = json.loads(GEO_CACHE_FILE.read_text(encoding="utf-8"))
        if c.get("query") == name.lower():
            return (float(c["lat"]), float(c["lon"]), c.get("label") or name)
    except (OSError, json.JSONDecodeError, KeyError, TypeError, ValueError):
        pass

    hit = await _geocode(name)
    if hit is None:
        log(f"Could not geocode '{name}'; using default location")
        return DEFAULT_LOCATION

    try:
        GEO_CACHE_FILE.parent.mkdir(parents=True, exist_ok=True)
        GEO_CACHE_FILE.write_text(json.dumps(
            {"query": name.lower(), "lat": hit[0], "lon": hit[1], "label": hit[2]}),
            encoding="utf-8")
    except OSError:
        pass
    return hit


async def poll_env() -> dict:
    """Clock + local weather for the device's Clock screen (src='env').

    Time is always included; weather is best-effort (open-meteo, keyless).
    """
    now = datetime.now().astimezone()
    result: dict = {
        "src": "env",
        "ts": int(time.time()),
        "tz": int(now.utcoffset().total_seconds() // 60) if now.utcoffset() else 0,
    }

    lat, lon, city = await _resolve_location()
    city = _ascii(city)[:15] or "Weather"
    try:
        async with httpx.AsyncClient(timeout=15.0) as http:
            resp = await http.get(
                "https://api.open-meteo.com/v1/forecast",
                params={
                    "latitude": round(lat, 3), "longitude": round(lon, 3),
                    "current": "temperature_2m,weather_code",
                    "daily": "temperature_2m_max,temperature_2m_min",
                    "forecast_days": 1, "timezone": "auto",
                },
            )
        d = resp.json()
        cur = d.get("current") or {}
        daily = d.get("daily") or {}
        result["tp"] = round(cur.get("temperature_2m", 0))
        result["tc"] = int(cur.get("weather_code", 0))
        result["th"] = round((daily.get("temperature_2m_max") or [0])[0])
        result["tl"] = round((daily.get("temperature_2m_min") or [0])[0])
        result["tn"] = city
        log(f"Env: {city} {result['tp']}C code={result['tc']} "
            f"H{result['th']} L{result['tl']}")
    except (httpx.HTTPError, KeyError, ValueError, IndexError) as e:
        log(f"Weather fetch failed: {e}")
        result["tn"] = city
    return result


def _read_activity_lines() -> list[tuple[int, str, str]]:
    try:
        raw = ACTIVITY_LOG.read_text(encoding="utf-8", errors="ignore").splitlines()[-80:]
    except OSError:
        return []
    out: list[tuple[int, str, str]] = []
    for line in raw:
        p = line.split("\t")
        if len(p) >= 3:
            try:
                out.append((int(p[0]), p[1], p[2]))
            except ValueError:
                continue
    return out


def _activity_from_transcripts(now: float) -> dict:
    """Fallback signal when the Claude Code hooks aren't installed: transcript
    file mtimes. Coarser (no 'needs input') but zero setup."""
    try:
        mtimes = sorted((f.stat().st_mtime for f in CLAUDE_PROJECTS_DIR.rglob("*.jsonl")),
                        reverse=True)
    except OSError:
        mtimes = []
    if not mtimes:
        return {"src": "act", "st": "idle", "n": 0, "age": -1}
    age = int(now - mtimes[0])
    agents = sum(1 for m in mtimes if now - m < 900)
    st = "working" if age < 25 else "done" if age < 150 else "idle"
    return {"src": "act", "st": st, "n": agents, "age": age}


def poll_activity() -> dict:
    """Claude Code activity for the device: working / idle / needs_input / done.

    Primary source is the hook log (install_hooks.py); falls back to transcript
    mtimes. Returns a src='act' payload.
    """
    now = time.time()
    lines = _read_activity_lines()
    if not lines:
        return _activity_from_transcripts(now)

    latest: dict[str, tuple[int, str]] = {}
    for ts, event, sid in lines:
        if sid not in latest or ts >= latest[sid][0]:
            latest[sid] = (ts, event)

    active = {s: v for s, v in latest.items() if now - v[0] < 900}
    agents = len(active)
    newest_ts = max((v[0] for v in latest.values()), default=0)
    age = int(now - newest_ts) if newest_ts else -1

    if any(ev == "Notification" and now - ts < 600 for ts, ev in active.values()):
        st = "needs_input"
    elif any(ev in ("UserPromptSubmit", "PreToolUse", "PostToolUse")
             for _ts, ev in active.values()):
        st = "working"
    elif any(ev in ("Stop", "SubagentStop") and now - ts < 120
             for ts, ev in latest.values()):
        st = "done"
    else:
        st = "idle"

    return {"src": "act", "st": st, "n": agents, "age": age}


def _run(args: list[str], cwd: Path | None = None, timeout: float = 10.0) -> str | None:
    try:
        r = subprocess.run(args, cwd=str(cwd) if cwd else None, capture_output=True,
                           text=True, timeout=timeout, creationflags=_NO_WINDOW)
        return r.stdout.strip() if r.returncode == 0 else None
    except (FileNotFoundError, subprocess.TimeoutExpired, OSError):
        return None


def _active_repo() -> Path | None:
    """The repo the developer is currently in: newest ~/.claude/ide/*.lock
    workspace, else the newest transcript's cwd."""
    ide = Path.home() / ".claude" / "ide"
    try:
        locks = sorted(ide.glob("*.lock"), key=lambda p: p.stat().st_mtime, reverse=True)
    except OSError:
        locks = []
    for lk in locks:
        try:
            wf = (json.loads(lk.read_text(encoding="utf-8")).get("workspaceFolders") or [])
            if wf and Path(wf[0]).exists():
                return Path(wf[0])
        except (OSError, json.JSONDecodeError, IndexError, TypeError):
            continue
    try:
        files = sorted(CLAUDE_PROJECTS_DIR.rglob("*.jsonl"),
                       key=lambda p: p.stat().st_mtime, reverse=True)[:3]
    except OSError:
        files = []
    for f in files:
        for line in reversed(f.read_text(encoding="utf-8", errors="ignore").splitlines()[-50:]):
            try:
                cwd = json.loads(line).get("cwd")
            except (json.JSONDecodeError, ValueError):
                continue
            if cwd and Path(cwd).exists():
                return Path(cwd)
    return None


def _newest_transcript() -> Path | None:
    try:
        files = sorted(CLAUDE_PROJECTS_DIR.rglob("*.jsonl"),
                       key=lambda p: p.stat().st_mtime, reverse=True)
        return files[0] if files else None
    except OSError:
        return None


def _model_short(model: str) -> str:
    m = (model or "").lower()
    for key in ("opus", "sonnet", "haiku", "fable", "mythos"):
        if key in m:
            return key.capitalize()
    return ""


def active_session_model_ctx() -> tuple[str, int]:
    """(short model name, context-window % used) from the newest transcript's
    last assistant message. ('', -1) when unavailable."""
    f = _newest_transcript()
    if not f:
        return ("", -1)
    try:
        lines = f.read_text(encoding="utf-8", errors="ignore").splitlines()[-60:]
    except OSError:
        return ("", -1)
    for line in reversed(lines):
        try:
            j = json.loads(line)
        except (json.JSONDecodeError, ValueError):
            continue
        msg = j.get("message") or {}
        if j.get("type") == "assistant" and msg.get("usage"):
            u = msg["usage"]
            ctx = (u.get("input_tokens", 0) + u.get("cache_read_input_tokens", 0)
                   + u.get("cache_creation_input_tokens", 0))
            pct = min(100, round(100 * ctx / CLAUDE_CTX_WINDOW)) if ctx else -1
            return (_model_short(msg.get("model", "")), pct)
    return ("", -1)


def poll_ci() -> dict | None:
    """CI run status + review queue + working tree for the active repo."""
    repo = _active_repo()
    if repo is None:
        return None
    result: dict = {"src": "ci", "state": "none", "wf": "", "br": "",
                    "age": -1, "rev": 0, "chg": 0, "dty": -1, "ah": 0, "bh": 0, "cf": False}

    # --- git working tree ---
    porc = _run(["git", "status", "--porcelain=v2", "--branch"], cwd=repo, timeout=6)
    if porc is not None:
        dirty = conflict = 0
        for ln in porc.splitlines():
            if ln.startswith("# branch.head "):
                result["br"] = ln.split(" ", 2)[2]
            elif ln.startswith("# branch.ab "):
                parts = ln.split()
                try:
                    result["ah"], result["bh"] = int(parts[2]), -int(parts[3])
                except (IndexError, ValueError):
                    pass
            elif ln and ln[0] in "12u":
                dirty += 1
                if ln[0] == "u":
                    conflict += 1
        result["dty"] = dirty
        result["cf"] = conflict > 0

    # --- GitHub CI + PRs (needs gh + a GitHub remote) ---
    runs = _run(["gh", "run", "list", "-L", "1", "--json",
                 "status,conclusion,workflowName,startedAt"], cwd=repo, timeout=12)
    if runs:
        try:
            arr = json.loads(runs)
            if arr:
                r = arr[0]
                if r.get("status") != "completed":
                    result["state"] = "running"
                elif r.get("conclusion") == "success":
                    result["state"] = "pass"
                else:
                    result["state"] = "fail"
                result["wf"] = (r.get("workflowName") or "")[:18]
                started = r.get("startedAt") or ""
                if started:
                    dt = datetime.fromisoformat(started.replace("Z", "+00:00"))
                    result["age"] = max(0, int((datetime.now(timezone.utc) - dt).total_seconds() / 60))
        except (json.JSONDecodeError, ValueError):
            pass

    mine = _run(["gh", "pr", "list", "--author", "@me", "--state", "open", "--json",
                 "reviewDecision,statusCheckRollup"], cwd=repo, timeout=12)
    if mine:
        try:
            for pr in json.loads(mine):
                rollup = pr.get("statusCheckRollup") or []
                failing = any(c.get("conclusion") in ("FAILURE", "TIMED_OUT", "CANCELLED")
                              for c in rollup)
                if failing or pr.get("reviewDecision") == "CHANGES_REQUESTED":
                    result["chg"] += 1
        except (json.JSONDecodeError, ValueError):
            pass

    tor = _run(["gh", "search", "prs", "--review-requested=@me", "--state=open",
                "-L", "40", "--json", "url"], timeout=12)
    if tor:
        try:
            result["rev"] = len(json.loads(tor))
        except (json.JSONDecodeError, ValueError):
            pass

    log(f"CI: {result['state']} {result['wf']} br={result['br']} "
        f"review={result['rev']} changes={result['chg']} dirty={result['dty']}")
    return result


def poll_summary() -> dict:
    """Today's totals from the transcripts + git, since local midnight."""
    now = datetime.now().astimezone()
    midnight = now.replace(hour=0, minute=0, second=0, microsecond=0)
    mid_ts = midnight.timestamp()

    minute_buckets: set[int] = set()
    tok = 0
    cost = 0.0
    try:
        files = list(CLAUDE_PROJECTS_DIR.rglob("*.jsonl"))
    except OSError:
        files = []
    for f in files:
        try:
            if f.stat().st_mtime < mid_ts:
                continue
            lines = f.read_text(encoding="utf-8", errors="ignore").splitlines()
        except OSError:
            continue
        for line in lines:
            try:
                j = json.loads(line)
            except (json.JSONDecodeError, ValueError):
                continue
            ts = j.get("timestamp", "")
            if not ts:
                continue
            try:
                t = datetime.fromisoformat(ts.replace("Z", "+00:00")).timestamp()
            except ValueError:
                continue
            if t < mid_ts:
                continue
            minute_buckets.add(int(t // 60))
            msg = j.get("message") or {}
            if j.get("type") == "assistant" and msg.get("usage"):
                u = msg["usage"]
                inp = u.get("input_tokens", 0) + u.get("cache_creation_input_tokens", 0)
                cr = u.get("cache_read_input_tokens", 0)
                out = u.get("output_tokens", 0)
                tok += inp + out   # fresh work; cache reads re-send the same tokens
                ri, ro = MODEL_RATES.get(_model_short(msg.get("model", "")).lower(), (0, 0))
                cost += (inp * ri + cr * ri * 0.1 + out * ro) / 1_000_000

    act_min = len(minute_buckets)
    commits = 0
    repo = _active_repo()
    if repo is not None:
        email = _run(["git", "config", "user.email"], cwd=repo, timeout=5) or ""
        out = _run(["git", "log", "--since", midnight.strftime("%Y-%m-%dT%H:%M:%S"),
                    f"--author={email}", "--oneline"], cwd=repo, timeout=8)
        if out is not None:
            commits = len([l for l in out.splitlines() if l.strip()])

    cp_used = -1
    cp = _last_payloads.get("copilot")
    if cp and cp.get("pe", -1) > 0 and cp.get("pr", -1) >= 0:
        cp_used = cp["pe"] - cp["pr"]

    result = {"src": "sum", "am": act_min, "tk": tok // 1000,
              "usd": round(cost), "cm": commits, "cp": cp_used}
    log(f"Summary: {act_min}min {tok // 1000}k ${round(cost)} {commits}commits cp={cp_used}")
    return result


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
    if isinstance(data, (dict, list)):
        # Recursively search nested JSON for a non-empty accessToken.
        def walk(node: object) -> str | None:
            if isinstance(node, dict):
                v = node.get("accessToken")
                if isinstance(v, str):
                    t = v.strip()
                    if t:
                        return t
                for child in node.values():
                    hit = walk(child)
                    if hit:
                        return hit
            elif isinstance(node, list):
                for child in node:
                    hit = walk(child)
                    if hit:
                        return hit
            return None

        token = walk(data)
        if token:
            return token
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


def read_config() -> dict[str, str]:
    """All `key = value` pairs from the daemon config file (lowercased keys)."""
    cfg: dict[str, str] = {}
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text(encoding="utf-8").splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                cfg[key.strip().lower()] = val.strip()
    except OSError:
        pass
    return cfg


def read_config_dirs() -> list[Path]:
    """Claude config dirs from config_dirs in daemon config (comma-separated).

    Defaults to [~/.claude] when unset.
    """
    raw = read_config().get("config_dirs", "")

    if not raw:
        return [DEFAULT_CONFIG_DIR]

    dirs = [Path(p.strip()).expanduser() for p in raw.split(",") if p.strip()]
    return dirs or [DEFAULT_CONFIG_DIR]


def _read_token_file(config_dir: Path) -> str | None:
    cred_file = config_dir / ".credentials.json"
    try:
        raw = cred_file.read_text(encoding="utf-8")
    except OSError as e:
        log(f"Error reading credentials in {config_dir}: {e}")
        return None
    return _extract_access_token(raw)


def _read_token_env() -> str | None:
    for env_name in TOKEN_ENV_VARS:
        val = os.getenv(env_name, "").strip()
        if val:
            return val
    return None


def _find_oauth_node(data: object) -> dict | None:
    """The dict holding the *Claude* accessToken + refreshToken. Skips the
    per-server nodes under mcpOAuth (those have accessToken but no refreshToken)."""
    if not isinstance(data, dict):
        return None
    n = data.get("claudeAiOauth")
    if isinstance(n, dict) and n.get("refreshToken"):
        return n
    if data.get("refreshToken") and data.get("accessToken"):
        return data
    for v in data.values():
        hit = _find_oauth_node(v)
        if hit is not None:
            return hit
    return None


async def refresh_oauth_token(config_dir: Path) -> str | None:
    """Exchange the on-disk refresh token for a fresh access token and write the
    rotated pair back to <config_dir>/.credentials.json. Returns the new access
    token, or None if there's nothing to refresh / the exchange failed."""
    cred_file = config_dir / ".credentials.json"
    try:
        blob = json.loads(cred_file.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    node = _find_oauth_node(blob)
    if not node or not node.get("refreshToken"):
        return None

    body = {
        "grant_type": "refresh_token",
        "refresh_token": node["refreshToken"],
        "client_id": OAUTH_CLIENT_ID,
    }
    tok = None
    for url in OAUTH_TOKEN_URLS:
        try:
            async with httpx.AsyncClient(timeout=20.0) as http:
                r = await http.post(url, json=body,
                                    headers={"Content-Type": "application/json",
                                             "User-Agent": "anthropic"})
        except httpx.HTTPError as e:
            log(f"Token refresh ({url}): {e}")
            continue
        if r.status_code == 404:
            continue
        if r.status_code != 200:
            log(f"Token refresh HTTP {r.status_code}: {r.text[:160]}")
            return None
        tok = r.json()
        break
    if not tok or "access_token" not in tok:
        return None

    node["accessToken"] = tok["access_token"]
    if tok.get("refresh_token"):
        node["refreshToken"] = tok["refresh_token"]
    if tok.get("expires_in"):
        node["expiresAt"] = int(time.time() * 1000) + int(tok["expires_in"]) * 1000

    try:
        bak = cred_file.with_suffix(".json.bak")
        if cred_file.exists() and not bak.exists():
            bak.write_text(cred_file.read_text(encoding="utf-8"), encoding="utf-8")
        tmp = cred_file.with_suffix(".json.tmp")
        tmp.write_text(json.dumps(blob, indent=2), encoding="utf-8")
        os.replace(tmp, cred_file)
        try:
            os.chmod(cred_file, 0o600)
        except OSError:
            pass
        log("Refreshed the Claude OAuth token")
    except OSError as e:
        log(f"Token refresh: could not write credentials: {e}")
    return tok["access_token"]


def read_token_for(config_dir: Path) -> str | None:
    token = _read_token_env()
    if token:
        return token

    token = _read_token_file(config_dir)
    if token:
        return token

    # Preserve the original macOS behavior: default profile can come from
    # Keychain with no on-disk token file.
    if sys.platform == "darwin" and config_dir == DEFAULT_CONFIG_DIR:
        return _read_token_keychain()

    return None


def read_token() -> str | None:
    global _missing_token_logged

    # Keep this helper for compatibility; first non-empty token wins.
    for cfg_dir in read_config_dirs():
        token = read_token_for(cfg_dir)
        if token:
            return token

    if not _missing_token_logged:
        log(
            "No Claude OAuth token found. Expected a non-empty accessToken in "
            "<config_dir>/.credentials.json for config_dirs from "
            f"{CONFIG_FILE} (default {DEFAULT_CONFIG_DIR}) or one of "
            f"{', '.join(TOKEN_ENV_VARS)}"
        )
        _missing_token_logged = True
    return None


async def poll_active_payload() -> dict | None:
    """Poll configured Claude config dirs and return the first valid payload.

    On a 401 from a file-based token, refresh it via the OAuth refresh token and
    retry once — so usage keeps flowing even when `claude` hasn't run lately."""
    env_token = _read_token_env()
    for cfg_dir in read_config_dirs():
        token = read_token_for(cfg_dir)
        if not token:
            continue
        payload = await poll_api(token)
        if payload is not None:
            return payload
        if _LAST_API_STATUS == 401 and not env_token:
            new_token = await refresh_oauth_token(cfg_dir)
            if new_token:
                payload = await poll_api(new_token)
                if payload is not None:
                    return payload
    return None


def have_any_token() -> bool:
    """True if a Claude token is resolvable from any configured source."""
    if _read_token_env():
        return True
    for cfg_dir in read_config_dirs():
        if read_token_for(cfg_dir):
            return True
    return False


def status_payload(state: str) -> dict:
    """A lightweight frame so the device can show *why* usage stopped updating."""
    return {"src": "status", "state": state}


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


# Last successfully-sent payload per `src`, so a display that reconnects after a
# daemon restart isn't stuck on stale numbers until the next poll cycle.
_last_payloads: dict[str, dict] = {}


def load_last_payloads() -> None:
    global _last_payloads
    try:
        data = json.loads(LAST_PAYLOAD_FILE.read_text(encoding="utf-8"))
        if isinstance(data, dict):
            _last_payloads = {k: v for k, v in data.items() if isinstance(v, dict)}
    except (OSError, json.JSONDecodeError):
        _last_payloads = {}


def remember_payload(payload: dict) -> None:
    src = payload.get("src", "claude")
    if src in ("status", "act"):
        return  # transient, not worth replaying
    _last_payloads[src] = payload
    try:
        LAST_PAYLOAD_FILE.parent.mkdir(parents=True, exist_ok=True)
        LAST_PAYLOAD_FILE.write_text(json.dumps(_last_payloads), encoding="utf-8")
    except OSError:
        pass


async def scan_for_device() -> str | None:
    log(f"Scanning for '{DEVICE_NAME}' ({SCAN_TIMEOUT}s)...")
    devices = await BleakScanner.discover(timeout=SCAN_TIMEOUT)
    for d in devices:
        if d.name == DEVICE_NAME:
            log(f"Found: {d.address}")
            return d.address
    return None


async def poll_api(token: str) -> dict | None:
    global _LAST_API_STATUS
    _LAST_API_STATUS = 0
    headers = dict(API_HEADERS_TEMPLATE)
    headers["Authorization"] = f"Bearer {token}"
    try:
        async with httpx.AsyncClient(timeout=20.0) as http:
            resp = await http.post(API_URL, headers=headers, json=API_BODY)
    except httpx.HTTPError as e:
        log(f"API call failed: {e}")
        return None
    _LAST_API_STATUS = resp.status_code

    def hdr(name: str, default: str = "0") -> str:
        return resp.headers.get(name, default)

    # Even on 429/other non-2xx, Anthropic returns ratelimit headers that are
    # sufficient to render usage + reset timers on-device. Keep using them.
    s5h_u = hdr("anthropic-ratelimit-unified-5h-utilization", "")
    s5h_r = hdr("anthropic-ratelimit-unified-5h-reset", "")
    s7d_u = hdr("anthropic-ratelimit-unified-7d-utilization", "")
    s7d_r = hdr("anthropic-ratelimit-unified-7d-reset", "")
    if resp.status_code >= 400:
        log(f"API HTTP {resp.status_code}: {resp.text[:200]}")
        if not (s5h_u and s5h_r and s7d_u and s7d_r):
            return None

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
        "s": pct(s5h_u),
        "sr": reset_minutes(s5h_r),
        "w": pct(s7d_u),
        "wr": reset_minutes(s7d_r),
        "st": hdr("anthropic-ratelimit-unified-5h-status", "unknown"),
        "ok": resp.status_code < 400,
    }
    mdl, ctx = active_session_model_ctx()
    if mdl:
        payload["mdl"] = mdl
    if ctx >= 0:
        payload["ctx"] = ctx
    return payload


class Session:
    def __init__(self, client: BleakClient) -> None:
        self.client = client
        self.refresh_requested = asyncio.Event()
        self.write_fails = 0

    def link_broken(self) -> bool:
        """Connected but writes keep failing (missing RX characteristic / stale
        GATT table) — only a fresh connect, or a device power-cycle, fixes it."""
        return self.write_fails >= 4

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

        # Double‑check connection before writing
        if not self.client.is_connected:
            log("❌ BLE client not connected – cannot write")
            return False

        try:
            await self.client.write_gatt_char(RX_CHAR_UUID, data, response=False)
            self.write_fails = 0
            remember_payload(payload)
            return True
        except BleakError as e:
            self.write_fails += 1
            log(f"Write failed ({self.write_fails}): {e}")
            if self.link_broken():
                log("RX characteristic unreachable - dropping the link to "
                    "reconnect. If this repeats, power-cycle the device.")
                try:
                    await self.client.disconnect()
                except BleakError:
                    pass
            return False

    async def replay_last_payloads(self) -> None:
        """Re-send the last known values so a fresh reconnect isn't blank."""
        for src, payload in list(_last_payloads.items()):
            if await self.write_payload(payload):
                log(f"Replayed cached {src} payload")


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
    await session.replay_last_payloads()

    last_poll = 0.0
    last_copilot_poll = 0.0
    last_sysinfo_poll = 0.0
    last_vscode_poll = 0.0
    last_env_poll = 0.0
    last_act_poll = 0.0
    last_act_state: str | None = None
    last_act_sent = 0.0
    last_ci_poll = 0.0
    last_sum_poll = 0.0
    used_successfully = False

    # Warm up cpu_percent so the first non-blocking call returns a real value
    try:
        import psutil  # type: ignore[import-untyped]
        psutil.cpu_percent(interval=None)
    except ImportError:
        pass

    try:
        while client.is_connected and not stop_event.is_set() and not session.link_broken():
            now = time.time()
            elapsed = now - last_poll
            if session.refresh_requested.is_set() or elapsed >= POLL_INTERVAL:
                session.refresh_requested.clear()
                payload = await poll_active_payload()
                last_poll = time.time()
                if payload is None:
                    state = "no_token" if not have_any_token() else "api_error"
                    log(f"No Claude payload; sending status={state}")
                    await session.write_payload(status_payload(state))
                elif await session.write_payload(payload):
                    used_successfully = True

            try:
                await asyncio.wait_for(session.refresh_requested.wait(), timeout=TICK)
            except asyncio.TimeoutError:
                pass

            # GitHub Copilot poll every 5 minutes
            now = time.time()
            if now - last_copilot_poll >= COPILOT_POLL_INTERVAL:
                last_copilot_poll = now
                gh_token = read_github_token()
                if gh_token:
                    cp_payload = await poll_copilot(gh_token)
                    if cp_payload is not None:
                        await session.write_payload(cp_payload)
                else:
                    log("No GitHub token (install gh CLI and run 'gh auth login'); skipping Copilot poll")

            # System info poll every 30 seconds
            now = time.time()
            if now - last_sysinfo_poll >= SYSINFO_POLL_INTERVAL:
                last_sysinfo_poll = now
                si_payload = poll_sysinfo()
                await session.write_payload(si_payload)

            # VS Code stats poll every 30 seconds
            now = time.time()
            if now - last_vscode_poll >= VSCODE_POLL_INTERVAL:
                last_vscode_poll = now
                vs_payload = poll_vscode()
                await session.write_payload(vs_payload)

            # Clock + weather every 15 minutes (also once, right after connect)
            now = time.time()
            if now - last_env_poll >= ENV_POLL_INTERVAL:
                last_env_poll = now
                await session.write_payload(await poll_env())

            # Claude activity — check often, send on change or as a 60s keepalive
            now = time.time()
            if now - last_act_poll >= ACT_POLL_INTERVAL:
                last_act_poll = now
                act = poll_activity()
                if act["st"] != last_act_state or now - last_act_sent >= 60:
                    if act["st"] != last_act_state:
                        log(f"Activity: {act['st']} (agents={act['n']})")
                    last_act_state, last_act_sent = act["st"], now
                    await session.write_payload(act)

            # CI status + review queue + git — every 2 min
            now = time.time()
            if now - last_ci_poll >= CI_POLL_INTERVAL:
                last_ci_poll = now
                ci = await asyncio.to_thread(poll_ci)
                if ci is not None:
                    await session.write_payload(ci)

            # Daily summary — every 5 min
            now = time.time()
            if now - last_sum_poll >= SUM_POLL_INTERVAL:
                last_sum_poll = now
                await session.write_payload(await asyncio.to_thread(poll_summary))
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

    log("=== Claude Usage Tracker Daemon (BLE) ===")
    log(f"Poll interval: {POLL_INTERVAL}s")
    load_last_payloads()

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
    for i, a in enumerate(sys.argv[1:], 1):
        if a in ("--log", "-l") and i < len(sys.argv) - 1:
            _LOG_FILE = Path(sys.argv[i + 1]).expanduser()
        elif a.startswith("--log="):
            _LOG_FILE = Path(a.split("=", 1)[1]).expanduser()
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
