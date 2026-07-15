#!/bin/bash
# Claude Usage Tracker Daemon (BLE)
# Reads Claude Code OAuth token, polls usage via API, sends to ESP32 over BLE GATT.
# Auto-connects and reconnects to the Clawdmeter BLE device.
# Dependencies: curl, awk, bluetoothctl

DEVICE_NAME="Clawdmeter"
DEVICE_MAC="${DEVICE_MAC:-}"  # auto-discovered if empty
SERVICE_UUID="4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID="4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID="4c41555a-4465-7669-6365-000000000004"
POLL_INTERVAL=30
TICK=5
SAVED_MAC_FILE="$HOME/.config/claude-usage-monitor/ble-address"
CONFIG_FILE="$HOME/.config/claude-usage-monitor/config"
REFRESH_FLAG="/tmp/claude-usage-refresh-$$"
DBUS_DEST="org.bluez"
NOTIFY_PID=""
LAST_PAYLOAD=""

log() {
    echo "[$(date '+%H:%M:%S')] $1"
}

# --- Multi config-dir support ---------------------------------------------
# Claude Code can run against more than one config dir (e.g. ~/.claude for a
# personal plan and ~/.claude-work for a work plan, selected via
# CLAUDE_CONFIG_DIR). The daemon polls each configured dir's token every cycle
# and shows whichever plan is "active" (the one whose usage moved most recently
# — see poll()). Per-dir state persists across poll() calls for that decision.
declare -A PREV_S       # last session % seen per dir (detects a rise = activity)
declare -A LAST_ACTIVE  # poll-sequence number of the last observed rise (0 = never)
POLL_SEQ=0              # monotonic poll counter — recency ordering that's immune to
                        # wall-clock resolution and NTP steps (polls are 60s apart, but
                        # a counter is unambiguous even if two land in the same second)

# Read the `config_dirs` option: a comma-separated list of Claude config dirs.
# Defaults to "~/.claude" so existing single-plan setups are unchanged. Tildes
# and $HOME are expanded; blanks trimmed. Echoes one resolved dir per line.
read_config_dirs() {
    local raw=""
    if [ -f "$CONFIG_FILE" ]; then
        raw=$(grep -E '^[[:space:]]*config_dirs[[:space:]]*=' "$CONFIG_FILE" | tail -1 \
            | tr -d '\r' \
            | sed -E 's/^[[:space:]]*config_dirs[[:space:]]*=[[:space:]]*//; s/[[:space:]]*(#.*)?$//')
    fi
    [ -z "$raw" ] && raw="$HOME/.claude"
    local IFS=','
    local d
    for d in $raw; do
        d=$(echo "$d" | sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//')
        [ -z "$d" ] && continue
        case "$d" in
            "~")   d="$HOME" ;;
            "~/"*) d="$HOME/${d#\~/}" ;;
        esac
        echo "$d"
    done
}

# Read the OAuth access token from a specific config dir's credentials file.
# The file can hold many "accessToken" fields — one per OAuth integration (MCP
# servers, design tools, etc.) — so we must isolate the claudeAiOauth object
# first and pull ITS accessToken. A bare grep for "accessToken" would return
# every token concatenated, yielding an invalid Bearer header and constant 401s.
read_token_for() {
    local dir="$1"
    grep -o '"claudeAiOauth"[[:space:]]*:[[:space:]]*{[^}]*}' "$dir/.credentials.json" 2>/dev/null \
        | grep -o '"accessToken":"[^"]*"' | head -1 | cut -d'"' -f4
}

# Read the `chime` option from the config file. Echoes one of: off|on.
# Defaults to "off" so the device stays silent until the user opts in.
read_chime_setting() {
    local val=""
    if [ -f "$CONFIG_FILE" ]; then
        val=$(grep -E '^[[:space:]]*chime[[:space:]]*=' "$CONFIG_FILE" | tail -1 \
            | tr -d '\r' \
            | sed -E 's/^[[:space:]]*chime[[:space:]]*=[[:space:]]*//; s/[[:space:]]*(#.*)?$//' \
            | tr '[:upper:]' '[:lower:]')
    fi
    case "$val" in
        on) echo "on" ;;
        *)  echo "off" ;;
    esac
}

# Read the `clock` option from the config file. Echoes one of: off|auto|12|24.
# Defaults to "off" so existing setups keep showing "Usage" until opted in.
read_clock_setting() {
    local val=""
    if [ -f "$CONFIG_FILE" ]; then
        val=$(grep -E '^[[:space:]]*clock[[:space:]]*=' "$CONFIG_FILE" | tail -1 \
            | tr -d '\r' \
            | sed -E 's/^[[:space:]]*clock[[:space:]]*=[[:space:]]*//; s/[[:space:]]*(#.*)?$//' \
            | tr '[:upper:]' '[:lower:]')
    fi
    case "$val" in
        off|auto|12|24) echo "$val" ;;
        *)              echo "off" ;;
    esac
}

# Best-effort 12h/24h detection from the locale. Echoes 12 or 24 (default 24).
detect_hour_format() {
    local tfmt
    tfmt=$(locale -k LC_TIME 2>/dev/null | grep -E '^t_fmt=')
    case "$tfmt" in
        *%p*|*%r*|*%I*) echo 12 ;;
        *)              echo 24 ;;
    esac
}

# Convert MAC to D-Bus path: AA:BB:CC:DD:EE:FF -> dev_AA_BB_CC_DD_EE_FF
mac_to_dbus_path() {
    local adapter
    adapter=$(busctl call org.bluez / org.freedesktop.DBus.ObjectManager GetManagedObjects 2>/dev/null | grep -o '/org/bluez/hci[0-9]' | head -1)
    adapter=${adapter:-/org/bluez/hci0}
    echo "${adapter}/dev_$(echo "$1" | tr ':' '_')"
}

# Check if device is connected via D-Bus
is_connected() {
    local path
    path=$(mac_to_dbus_path "$DEVICE_MAC")
    busctl get-property "$DBUS_DEST" "$path" org.bluez.Device1 Connected 2>/dev/null | grep -q "true"
}

# Load saved MAC address
load_mac() {
    if [ -n "$DEVICE_MAC" ]; then return 0; fi
    if [ -f "$SAVED_MAC_FILE" ]; then
        DEVICE_MAC=$(head -1 "$SAVED_MAC_FILE" | tr -d '\r\n ')
        if [[ "$DEVICE_MAC" =~ ^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$ ]]; then
            return 0
        fi
        log "Cached MAC is malformed, discarding"
        rm -f "$SAVED_MAC_FILE"
        DEVICE_MAC=""
    fi
    return 1
}

# Save MAC for fast reconnect
save_mac() {
    mkdir -p "$(dirname "$SAVED_MAC_FILE")"
    echo "$DEVICE_MAC" > "$SAVED_MAC_FILE"
}

# Find a Clawdmeter the system already knows about — paired first, then merely
# connected — WITHOUT an LE advertising scan. bluez only lists devices this host
# has bonded/connected to, so we can't accidentally grab a stranger's advertising
# unit. The device is a bonded BLE HID keyboard you pair once anyway, so we never
# scan by name. Sets DEVICE_MAC + caches it on success; returns non-zero if none.
find_system_device_mac() {
    local found=""
    local mode
    for mode in Paired Connected; do
        found=$(bluetoothctl devices "$mode" 2>/dev/null | grep "$DEVICE_NAME" | head -1 | awk '{print $2}')
        [ -n "$found" ] && break
    done
    if [ -n "$found" ]; then
        DEVICE_MAC="$found"
        save_mac
        log "Using system-known device: $DEVICE_MAC"
        return 0
    fi
    return 1
}

# Connect to the device
connect_device() {
    log "Connecting to $DEVICE_MAC..."

    # Trust first (allows auto-reconnect)
    bluetoothctl trust "$DEVICE_MAC" &>/dev/null

    # Connect
    bluetoothctl connect "$DEVICE_MAC" &>/dev/null
    sleep 2

    if is_connected; then
        log "Connected"
        return 0
    fi
    log "Connection failed"
    # Drop the cached MAC so the next loop re-derives it from bluez's paired/
    # connected list (see find_system_device_mac). We deliberately do NOT
    # `bluetoothctl remove` here: the daemon now only ever connects to a device
    # the system already knows, so unpairing it on a transient failure would
    # make it undiscoverable and strand the daemon.
    if [ -f "$SAVED_MAC_FILE" ] && [ "$(cat "$SAVED_MAC_FILE")" = "$DEVICE_MAC" ]; then
        log "Invalidating cached MAC, will re-derive from paired/connected devices"
        rm -f "$SAVED_MAC_FILE"
    fi
    DEVICE_MAC=""
    return 1
}

# Find a GATT characteristic path by UUID via D-Bus
find_char_path_by_uuid() {
    local target_uuid="$1"
    local dev_path
    dev_path=$(mac_to_dbus_path "$DEVICE_MAC")

    busctl tree "$DBUS_DEST" 2>/dev/null | grep -o "${dev_path}/service[0-9a-f]*/char[0-9a-f]*" | while read -r char_path; do
        local uuid
        uuid=$(busctl get-property "$DBUS_DEST" "$char_path" org.bluez.GattCharacteristic1 UUID 2>/dev/null | tr -d '"' | awk '{print $2}')
        if [ "$uuid" = "$target_uuid" ]; then
            echo "$char_path"
            return 0
        fi
    done
}

# Subscribe to refresh-request notifications. The ESP fires this when it
# has no usage data yet (e.g. after a fresh boot). Daemon awk drops a flag
# file that the inner loop picks up on its next 5s tick.
#
# Implementation notes:
# - dbus-monitor must be running BEFORE we call StartNotify, because busctl
#   exits immediately, the subscription tears down within milliseconds, and
#   the ESP's notify fires inside that brief window.
# - stdbuf -oL forces line-buffered stdout on dbus-monitor; without it,
#   glibc switches to block buffering when stdout is a pipe and signals
#   never reach awk until ~4KB accumulates.
# - The pipeline runs in a setsid'd child so we can kill the whole process
#   group (dbus-monitor + awk) atomically. Killing only awk leaves
#   dbus-monitor orphaned, and `wait $!` in bash waits on the whole job
#   until every pipeline member exits, hanging the daemon.
start_notify_subscriber() {
    local req_path
    req_path=$(find_char_path_by_uuid "$REQ_CHAR_UUID")
    if [ -z "$req_path" ]; then
        log "Refresh char not found, skipping notify subscriber"
        return 1
    fi

    setsid bash -c "stdbuf -oL dbus-monitor --system \"type='signal',interface='org.freedesktop.DBus.Properties',path='$req_path',member='PropertiesChanged'\" 2>/dev/null | awk -v flag='$REFRESH_FLAG' '/Value/ { system(\"touch \" flag); fflush() }'" &
    NOTIFY_PID=$!

    # Give dbus-monitor a moment to register its match rule, then trigger
    # the GATT subscription that causes the ESP to fire its notify.
    sleep 0.3
    busctl call "$DBUS_DEST" "$req_path" org.bluez.GattCharacteristic1 StartNotify >/dev/null 2>&1

    log "Refresh subscriber started (pgid=$NOTIFY_PID)"
}

stop_notify_subscriber() {
    if [ -n "$NOTIFY_PID" ]; then
        # Kill the whole process group (setsid made NOTIFY_PID the leader).
        # Don't wait — we don't care about exit status and waiting can hang
        # if any group member is slow to exit.
        kill -TERM -"$NOTIFY_PID" 2>/dev/null
        NOTIFY_PID=""
    fi
    rm -f "$REFRESH_FLAG"
}

# Write data to the RX characteristic via D-Bus
write_gatt() {
    local char_path="$1"
    local data="$2"

    # Convert string to byte array for D-Bus: "hi" -> 0x68 0x69
    local bytes=""
    for ((i = 0; i < ${#data}; i++)); do
        local byte
        byte=$(printf "0x%02x" "'${data:$i:1}")
        bytes="$bytes $byte"
    done
    local count=${#data}

    busctl call "$DBUS_DEST" "$char_path" org.bluez.GattCharacteristic1 \
        WriteValue "aya{sv}" "$count" $bytes 0 2>/dev/null
}

# Build the device payload for one OAuth token. Echoes the JSON payload on
# success (empty + non-zero return on failure). Pure: no logging, no GATT write
# — poll() owns picking the active plan and sending it.
build_payload_for_token() {
    local token="$1"
    [ -z "$token" ] && return 1
    local now
    now=$(date +%s)

    # Optional clock. When enabled, send a local wall-clock epoch (UTC epoch shifted
    # by the timezone offset, so gmtime() on-device reads local) plus the hour format.
    local clock clock_fragment=""
    clock=$(read_clock_setting)
    if [ "$clock" != "off" ]; then
        local tz off_sec local_epoch tf
        tz=$(date +%z)            # e.g. +0200 or -0500
        off_sec=$(( (10#${tz:1:2} * 3600) + (10#${tz:3:2} * 60) ))
        [ "${tz:0:1}" = "-" ] && off_sec=$(( -off_sec ))
        local_epoch=$(( now + off_sec ))
        case "$clock" in
            12) tf=12 ;;
            24) tf=24 ;;
            *)  tf=$(detect_hour_format) ;;
        esac
        clock_fragment=",\"t\":$local_epoch,\"tf\":$tf"
    fi

    local headers
    headers=$(curl -s -D - -o /dev/null \
        "https://api.anthropic.com/v1/messages" \
        -H "Authorization: Bearer $token" \
        -H "anthropic-version: 2023-06-01" \
        -H "anthropic-beta: oauth-2025-04-20" \
        -H "Content-Type: application/json" \
        -H "User-Agent: claude-code/2.1.5" \
        -d '{"model":"claude-haiku-4-5-20251001","max_tokens":1,"messages":[{"role":"user","content":"hi"}]}' \
        2>/dev/null) || return 1

    local s5h_util overage_util overage_reset status
    s5h_util=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-5h-utilization" | tr -d '\r' | awk '{print $2}')
    overage_util=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-overage-utilization" | tr -d '\r' | awk '{print $2}')
    overage_reset=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-overage-reset" | tr -d '\r' | awk '{print $2}')
    status=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-status" | tr -d '\r' | awk '{print $2}')
    status=${status:-unknown}

    # Optional reset chime. When enabled, tell the firmware it may sound the
    # session-reset chime by adding "c":1 to the payload (additive, off by default).
    local chime chime_fragment=""
    chime=$(read_chime_setting)
    [ "$chime" = "on" ] && chime_fragment=",\"c\":1"

    local payload
    if [ -n "$s5h_util" ]; then
        # Pro/Max account — 5h/7d windows
        local s7d_util s5h_reset s7d_reset s5h_status
        s5h_reset=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-5h-reset" | tr -d '\r' | awk '{print $2}')
        s7d_util=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-7d-utilization" | tr -d '\r' | awk '{print $2}')
        s7d_reset=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-7d-reset" | tr -d '\r' | awk '{print $2}')
        s5h_status=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-5h-status" | tr -d '\r' | awk '{print $2}')
        s5h_util=${s5h_util:-0}; s5h_reset=${s5h_reset:-0}
        s7d_util=${s7d_util:-0}; s7d_reset=${s7d_reset:-0}
        s5h_status=${s5h_status:-unknown}

        local plan_fragment="" plan_label=""
        plan_label=$(read_plan_label_for "$PAYLOAD_DIR")
        [ -n "$plan_label" ] && plan_fragment=",\"pl\":\"$plan_label\""
        # CODEX_FRAGMENT is computed once per poll cycle (see poll()) and is empty
        # whenever Codex data is unavailable — the firmware then renders the
        # Claude-only 2-bar view. CODEX_CONTEXT_FRAGMENT is local-only and fills
        # the second Codex bar when a rollout JSONL with token_count data exists.
        # Enterprise deliberately gets no Codex bar.
        payload=$(awk -v u5="$s5h_util" -v r5="$s5h_reset" -v u7="$s7d_util" -v r7="$s7d_reset" -v st="$s5h_status" -v now="$now" -v clk="$clock_fragment" -v chm="$chime_fragment" -v cdx="$CODEX_FRAGMENT" -v cctx="$CODEX_CONTEXT_FRAGMENT" -v agy="$ANTIGRAVITY_FRAGMENT" -v sys="$SYSTEM_FRAGMENT" -v pln="$plan_fragment" \
            'BEGIN {
                sp = sprintf("%.0f", u5 * 100);
                sr = (r5 - now) / 60; sr = sr > 0 ? sprintf("%.0f", sr) : 0;
                wp = sprintf("%.0f", u7 * 100);
                wr = (r7 - now) / 60; wr = wr > 0 ? sprintf("%.0f", wr) : 0;
                printf "{\"s\":%s,\"sr\":%s,\"w\":%s,\"wr\":%s,\"st\":\"%s\",\"acct\":\"pro\"%s%s%s%s%s%s%s,\"ok\":true}", sp, sr, wp, wr, st, clk, chm, cdx, cctx, agy, sys, pln;
            }')
    else
        # Enterprise account — spending-limit model
        overage_util=${overage_util:-0}; overage_reset=${overage_reset:-0}
        # Compute period info via python3 (awk lacks date arithmetic)
        local period_info
        period_info=$(python3 - "$now" "$overage_reset" <<'PYEOF'
import sys, datetime, calendar, json
now, reset_ts = float(sys.argv[1]), float(sys.argv[2])
dt_end = datetime.datetime.fromtimestamp(reset_ts)
pm = dt_end.month - 1 or 12
py = dt_end.year if dt_end.month > 1 else dt_end.year - 1
pd = min(dt_end.day, calendar.monthrange(py, pm)[1])
dt_start = dt_end.replace(year=py, month=pm, day=pd)
period_len = reset_ts - dt_start.timestamp()
tp = max(0, min(100, int(round((now - dt_start.timestamp()) / period_len * 100)))) if period_len > 0 else 0
pd_days = int(round(period_len / 86400))
rd = f"{dt_end.strftime('%b')} {dt_end.day}"
print(json.dumps({"tp": tp, "pd": pd_days, "rd": rd}))
PYEOF
)
        payload=$(awk -v ou="$overage_util" -v or_="$overage_reset" -v st="$status" -v now="$now" -v pi="$period_info" -v clk="$clock_fragment" -v chm="$chime_fragment" -v cctx="$CODEX_CONTEXT_FRAGMENT" -v agy="$ANTIGRAVITY_FRAGMENT" -v sys="$SYSTEM_FRAGMENT" \
            'BEGIN {
                sp = sprintf("%.0f", ou * 100);
                sr = (or_ - now) / 60; sr = sr > 0 ? sprintf("%.0f", sr) : 0;
                # Extract tp, pd, rd from period_info JSON (simple regex)
                tp = 0; pd = 30; rd = "";
                match(pi, /"tp": *([0-9]+)/, a); if (RSTART) tp = a[1];
                match(pi, /"pd": *([0-9]+)/, b); if (RSTART) pd = b[1];
                match(pi, /"rd": *"([^"]+)"/, c); if (RSTART) rd = c[1];
                printf "{\"s\":%s,\"sr\":%s,\"w\":0,\"wr\":0,\"st\":\"%s\",\"acct\":\"ent\",\"tp\":%s,\"pd\":%s,\"rd\":\"%s\"%s%s%s%s%s,\"ok\":true}", sp, sr, st, tp, pd, rd, clk, chm, cctx, agy, sys;
            }')
    fi

    printf '%s' "$payload"
    return 0
}

# Claude plan label for the subtitle, e.g. "Claude Max 20x". Source is
# claudeAiOauth.rateLimitTier ("default_claude_max_20x"); subscriptionType
# ("max") is the fallback since it lacks the multiplier. Echoes "" if unknown —
# the firmware then just leaves the subtitle blank rather than guessing.
read_plan_label_for() {
    local dir="$1"
    [ -f "$dir/.credentials.json" ] || return 0
    python3 -c '
import json, sys
try:
    o = json.load(open(sys.argv[1])).get("claudeAiOauth") or {}
except Exception:
    sys.exit(0)
tier = o.get("rateLimitTier") or ""
sub = o.get("subscriptionType") or ""
s = tier or sub
if not s:
    sys.exit(0)
for p in ("default_claude_", "default_"):
    if s.startswith(p):
        s = s[len(p):]
        break
s = s.replace("claude_", "").replace("_", " ").strip()
if not s:
    sys.exit(0)
# "max 20x" -> "Max 20x" (title() would give "20X")
words = [w if any(c.isdigit() for c in w) else w.capitalize() for w in s.split()]
sys.stdout.write("Claude " + " ".join(words))
' "$dir/.credentials.json" 2>/dev/null
}

# --- Host system resources -------------------------------------------------
# Compact, best-effort Linux metrics for the System screen. Percentages are
# always present; unavailable temperatures use -1. NVIDIA is preferred when
# available, with DRM/hwmon as the integrated-GPU fallback.
SYSTEM_FRAGMENT=""
system_resources_fragment() {
    python3 - "${SYSTEM_CPU_PCT:-}" "${SYSTEM_CPU_TEMP:-}" \
        "${SYSTEM_GPU_PCT:-}" "${SYSTEM_GPU_TEMP:-}" "${SYSTEM_RAM_PCT:-}" <<'PYEOF' 2>/dev/null
import glob, os, pathlib, subprocess, sys, time

overrides = sys.argv[1:]

def number(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None

def percent(value):
    value = number(value)
    return max(0, min(100, int(round(value)))) if value is not None else 0

def temperature(value):
    value = number(value)
    return int(round(value)) if value is not None else -1

def cpu_percent():
    def sample():
        fields = pathlib.Path("/proc/stat").read_text().splitlines()[0].split()[1:]
        values = [int(v) for v in fields]
        idle = values[3] + (values[4] if len(values) > 4 else 0)
        return sum(values), idle
    try:
        total1, idle1 = sample()
        time.sleep(0.1)
        total2, idle2 = sample()
        delta = total2 - total1
        return 100.0 * (delta - (idle2 - idle1)) / delta if delta > 0 else 0
    except (OSError, ValueError, IndexError):
        return 0

def ram_percent():
    try:
        values = {}
        for line in pathlib.Path("/proc/meminfo").read_text().splitlines():
            key, value = line.split(":", 1)
            values[key] = int(value.split()[0])
        total = values["MemTotal"]
        return 100.0 * (total - values["MemAvailable"]) / total
    except (OSError, KeyError, ValueError, ZeroDivisionError):
        return 0

def hwmon_temperature(kind):
    candidates = []
    for name_path in glob.glob("/sys/class/hwmon/hwmon*/name"):
        base = pathlib.Path(name_path).parent
        try:
            chip = pathlib.Path(name_path).read_text().strip().lower()
        except OSError:
            continue
        for input_path in base.glob("temp*_input"):
            label_path = input_path.with_name(input_path.name.replace("_input", "_label"))
            try:
                label = label_path.read_text().strip().lower() if label_path.exists() else ""
                temp = float(input_path.read_text().strip()) / 1000.0
            except (OSError, ValueError):
                continue
            if kind == "cpu":
                score = (100 if chip == "k10temp" and label == "tctl" else
                         90 if chip == "coretemp" and "package" in label else
                         80 if chip in ("k10temp", "coretemp") else
                         70 if "cpu" in label or "cpu" in chip else 0)
            else:
                score = (100 if chip in ("amdgpu", "nouveau") and label in ("edge", "junction", "") else
                         80 if chip in ("amdgpu", "nouveau") else 0)
            if score:
                candidates.append((score, temp))
    return max(candidates)[1] if candidates else None

def gpu_metrics():
    try:
        result = subprocess.run(
            ["nvidia-smi", "--query-gpu=utilization.gpu,temperature.gpu",
             "--format=csv,noheader,nounits"], capture_output=True, text=True,
            timeout=2, check=True)
        rows = []
        for line in result.stdout.splitlines():
            util, temp = (number(v.strip()) for v in line.split(",", 1))
            if util is not None:
                rows.append((util, temp))
        if rows:
            return max(rows, key=lambda row: row[0])
    except (OSError, subprocess.SubprocessError, ValueError):
        pass
    values = []
    for path in glob.glob("/sys/class/drm/card*/device/gpu_busy_percent"):
        try:
            values.append(float(pathlib.Path(path).read_text().strip()))
        except (OSError, ValueError):
            pass
    return (max(values) if values else 0, hwmon_temperature("gpu"))

gpu_pct, gpu_temp = gpu_metrics()
cpu_pct = number(overrides[0]) if overrides[0] else cpu_percent()
cpu_temp = number(overrides[1]) if overrides[1] else hwmon_temperature("cpu")
gpu_pct = number(overrides[2]) if overrides[2] else gpu_pct
gpu_temp = number(overrides[3]) if overrides[3] else gpu_temp
ram_pct = number(overrides[4]) if overrides[4] else ram_percent()

print(',"cpu":%d,"ct":%d,"gpu":%d,"gt":%d,"ram":%d' % (
    percent(cpu_pct), temperature(cpu_temp), percent(gpu_pct),
    temperature(gpu_temp), percent(ram_pct)), end="")
PYEOF
}

# --- Codex (OpenAI) usage -------------------------------------------------
# Codex quota is read from the ChatGPT backend endpoint the Codex CLI itself
# polls, authenticated with the OAuth access token the CLI stores. Notes:
#   * We only ever READ auth.json. The CLI owns token refresh (a 240h JWT plus a
#     refresh_token); a second writer would race it over a credentials file and
#     could log the user out.
#   * The endpoint is undocumented and may change without notice — every failure
#     path here is non-fatal and simply omits the Codex keys.
#   * OpenAI's published usage APIs are NOT usable: they cover pay-per-token API
#     keys only and cannot see ChatGPT-subscription Codex usage.
CODEX_AUTH_FILE="${CODEX_HOME:-$HOME/.codex}/auth.json"
CODEX_USAGE_URL="https://chatgpt.com/backend-api/wham/usage"
CODEX_FRAGMENT=""   # recomputed once per poll cycle; "" = no Codex data this cycle
CODEX_CONTEXT_FRAGMENT=""   # local Codex session context; "" = unavailable
PAYLOAD_DIR=""      # config dir the in-flight payload belongs to (plan label lookup)
# Last good Codex reading, reused across a transient failure. Measured response
# times for this endpoint range ~0.9s to ~6.6s, so an occasional slow reply trips
# the curl timeout; without this the panel would flap to "No Codex data" for a
# cycle and back. Quota moves slowly (a 7-day window), so serving a reading up to
# CODEX_STALE_MAX old is far better than blanking.
# ponytail: reused verbatim, so the countdown can lag by up to CODEX_STALE_MAX.
# Invisible on a weekly window; revisit if a short (e.g. 5h) window ever ships.
CODEX_LAST_FRAGMENT=""
CODEX_LAST_TS=0
CODEX_STALE_MAX=600   # 10 min; past this, Codex is treated as genuinely gone

read_codex_token() {
    [ -f "$CODEX_AUTH_FILE" ] || return 1
    python3 -c '
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    sys.exit(1)
sys.stdout.write((d.get("tokens") or {}).get("access_token") or "")
' "$CODEX_AUTH_FILE" 2>/dev/null
}

# Echo a JSON fragment (",\"cx\":..,\"cxr\":..,\"cxw\":..") or nothing at all.
# Never returns non-zero: Codex is strictly optional and must never be able to
# fail the Claude payload or stall the poll loop. max-time must stay well under
# POLL_INTERVAL; the endpoint has been measured at ~0.9-6.6s.
codex_fetch_fragment() {
    local token json
    token=$(read_codex_token) || return 0
    [ -z "$token" ] && return 0
    json=$(curl -s --connect-timeout 5 --max-time 15 "$CODEX_USAGE_URL" \
        -H "Authorization: Bearer $token" \
        -H "Accept: application/json" 2>/dev/null) || return 0
    [ -z "$json" ] && return 0
    printf '%s' "$json" | python3 -c '
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    sys.exit(0)
rl = d.get("rate_limit") or {}
# ponytail: show the most-used window. Plus exposes only primary_window (7d) and
# leaves secondary_window null, but the pair exists in the API — picking the
# most-used one renders whichever limit you will hit first. Revisit if a plan
# ever needs both windows on screen at once.
best = None
for k in ("primary_window", "secondary_window"):
    w = rl.get(k)
    if not isinstance(w, dict) or w.get("used_percent") is None:
        continue
    if best is None or w["used_percent"] > best["used_percent"]:
        best = w
if best is None:
    sys.exit(0)
try:
    pct = int(round(float(best["used_percent"])))
except (TypeError, ValueError):
    sys.exit(0)
reset_s = best.get("reset_after_seconds")
win_s = best.get("limit_window_seconds")
cxr = max(0, int(round(reset_s / 60))) if isinstance(reset_s, (int, float)) else -1
cxw = int(round(win_s / 60)) if isinstance(win_s, (int, float)) and win_s else 10080
out = ",\"cx\":%d,\"cxr\":%d,\"cxw\":%d" % (pct, cxr, cxw)

plan = d.get("plan_type")
if isinstance(plan, str) and plan:
    out += ",\"cxpl\":\"Codex %s\"" % plan.replace("_", " ").title()
sys.stdout.write(out)
' 2>/dev/null
}

# Latest Codex context window from the newest rollout JSONL that has a
# token_count event. This is local-only and independent from the Codex quota
# API; it gives the second Codex bar a real "context" reading.
codex_context_fragment() {
    local home="${CODEX_HOME:-$HOME/.codex}"
    [ -d "$home" ] || return 0
    python3 - "$home" <<'PYEOF' 2>/dev/null
import glob, json, os, sys

home = sys.argv[1]
files = glob.glob(os.path.join(home, "sessions", "**", "rollout-*.jsonl"), recursive=True) \
      + glob.glob(os.path.join(home, "archived_sessions", "*.jsonl"))
if not files:
    sys.exit(0)

for f in sorted(files, key=lambda p: os.path.getmtime(p), reverse=True):
    last = None
    try:
        fh = open(f)
    except OSError:
        continue
    with fh:
        for line in fh:
            try:
                rec = json.loads(line)
            except Exception:
                continue
            p = rec.get("payload") or {}
            if p.get("type") == "token_count":
                last = p
    if not last:
        continue
    info = last.get("info") or {}
    usage = info.get("last_token_usage") or info.get("total_token_usage") or {}
    tokens = usage.get("total_tokens")
    window = info.get("model_context_window")
    if isinstance(tokens, (int, float)) and isinstance(window, (int, float)) and window > 0:
        sys.stdout.write(",\"ctx\":%d,\"ctxw\":%d" % (int(tokens), int(window)))
        sys.exit(0)
sys.exit(0)
PYEOF
}

# Refresh CODEX_FRAGMENT for this cycle. Sets globals rather than echoing —
# command substitution would run this in a subshell and silently discard the
# cache updates below.
poll_codex() {
    local frag now_ts age
    frag=$(codex_fetch_fragment)
    now_ts=$(date +%s)

    if [ -n "$frag" ]; then
        CODEX_FRAGMENT="$frag"
        CODEX_LAST_FRAGMENT="$frag"
        CODEX_LAST_TS="$now_ts"
        return 0
    fi

    # Transient failure — serve the last good reading rather than blanking.
    if [ -n "$CODEX_LAST_FRAGMENT" ]; then
        age=$(( now_ts - CODEX_LAST_TS ))
        if [ "$age" -lt "$CODEX_STALE_MAX" ]; then
            CODEX_FRAGMENT="$CODEX_LAST_FRAGMENT"
            log "Codex poll failed; reusing reading from ${age}s ago"
            return 0
        fi
        log "Codex unavailable for ${age}s (> ${CODEX_STALE_MAX}s) — dropping the Codex panel"
        CODEX_LAST_FRAGMENT=""
    fi
    CODEX_FRAGMENT=""
    return 0
}

refresh_cached_payload() {
    local system_json payload
    [ -n "$LAST_PAYLOAD" ] || return 0
    system_json=$(system_resources_fragment)
    [ -n "$system_json" ] || return 0
    system_json="{${system_json#,}}"
    payload=$(python3 - "$LAST_PAYLOAD" "$system_json" <<'PYEOF' 2>/dev/null
import json, sys

try:
    payload = json.loads(sys.argv[1])
    system = json.loads(sys.argv[2])
except Exception:
    sys.exit(0)

for key in ("cpu", "ct", "gpu", "gt", "ram"):
    if key in system:
        payload[key] = system[key]

sys.stdout.write(json.dumps(payload, separators=(",", ":")))
PYEOF
)
    [ -n "$payload" ] || return 0
    LAST_PAYLOAD="$payload"
    write_gatt "$RX_CHAR_PATH" "$payload" || log "System refresh write failed"
    return 0
}

# --- Antigravity CLI (Google) usage ----------------------------------------
# `agy` exposes the same quota summary used by its /usage screen through its
# loopback language server. The port is random on every launch and is recorded
# in the CLI log together with the owning PID. We only query a port whose PID is
# still alive. A successful response is persisted so closing the CLI does not
# make an authenticated user look signed out; its absolute reset timestamps are
# converted again on every poll, keeping the countdowns honest while offline.
ANTIGRAVITY_HOME="${ANTIGRAVITY_HOME:-$HOME/.gemini/antigravity-cli}"
ANTIGRAVITY_CACHE_FILE="${ANTIGRAVITY_CACHE_FILE:-${XDG_CACHE_HOME:-$HOME/.cache}/clawdmeter/antigravity-quota.json}"
ANTIGRAVITY_FRAGMENT=""
ANTIGRAVITY_LAST_FRAGMENT=""
ANTIGRAVITY_LAST_TS=0
ANTIGRAVITY_STALE_MAX=600

find_antigravity_http_port() {
    if [ -n "${ANTIGRAVITY_HTTP_PORT:-}" ]; then
        printf '%s' "$ANTIGRAVITY_HTTP_PORT"
        return 0
    fi
    local log_file line pid port
    for log_file in $(ls -1t "$ANTIGRAVITY_HOME"/log/cli-*.log 2>/dev/null); do
        line=$(grep -m1 'Language server listening on random port at .* for HTTP$' "$log_file" 2>/dev/null) || continue
        pid=$(printf '%s\n' "$line" | awk '{print $3}')
        port=$(printf '%s\n' "$line" | sed -n 's/.* port at \([0-9][0-9]*\) for HTTP$/\1/p')
        [ -n "$port" ] || continue
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            printf '%s' "$port"
            return 0
        fi
    done
    return 1
}

# Convert a quota response into the compact Gemini-pool fragment sent over BLE.
# Antigravity also returns a separate "Claude and GPT models" pool; mixing those
# independent quotas into a Gemini-branded tab would lie, so it is ignored.
antigravity_fragment_from_json() {
    local now_ts
    now_ts=${ANTIGRAVITY_NOW:-$(date +%s)}
    python3 -c '
import datetime, json, sys
try:
    d = json.load(sys.stdin)
    now = float(sys.argv[1])
except Exception:
    sys.exit(0)

group = next((g for g in (d.get("response") or {}).get("groups", [])
              if g.get("displayName") == "Gemini Models"), None)
if not group:
    sys.exit(0)
buckets = {b.get("window"): b for b in group.get("buckets", [])
           if isinstance(b, dict)}
if "5h" not in buckets or "weekly" not in buckets:
    sys.exit(0)

def values(bucket):
    try:
        used = int(round((1.0 - float(bucket["remainingFraction"])) * 100))
        used = max(0, min(100, used))
        reset = datetime.datetime.fromisoformat(bucket["resetTime"].replace("Z", "+00:00")).timestamp()
        mins = max(0, int(round((reset - now) / 60)))
        return used, mins
    except Exception:
        raise SystemExit

five, five_reset = values(buckets["5h"])
week, week_reset = values(buckets["weekly"])
sys.stdout.write(",\"ag5\":%d,\"ag5r\":%d,\"agw\":%d,\"agwr\":%d,\"agpl\":\"Gemini Models\"" %
                 (five, five_reset, week, week_reset))
' "$now_ts" 2>/dev/null
}

# Prefer the live language server, but fall back to the last valid response on
# disk when agy is closed. Storing the raw response (rather than the BLE
# fragment) preserves absolute reset timestamps so reset minutes keep ticking.
antigravity_fetch_fragment() {
    local port json frag cache_dir cache_tmp
    port=$(find_antigravity_http_port 2>/dev/null) || port=""
    if [ -n "$port" ]; then
        json=$(curl -s --connect-timeout 1 --max-time 3 -X POST \
            "http://127.0.0.1:${port}/exa.language_server_pb.LanguageServerService/RetrieveUserQuotaSummary" \
            -H "Content-Type: application/json" -d '{}' 2>/dev/null) || json=""
        if [ -n "$json" ]; then
            frag=$(printf '%s' "$json" | antigravity_fragment_from_json)
            if [ -n "$frag" ]; then
                cache_dir=$(dirname "$ANTIGRAVITY_CACHE_FILE")
                cache_tmp="${ANTIGRAVITY_CACHE_FILE}.tmp.$$"
                (umask 077; mkdir -p "$cache_dir" && printf '%s' "$json" > "$cache_tmp" &&
                    mv -f "$cache_tmp" "$ANTIGRAVITY_CACHE_FILE") 2>/dev/null ||
                    rm -f "$cache_tmp"
                printf '%s' "$frag"
                return 0
            fi
        fi
    fi

    if [ -r "$ANTIGRAVITY_CACHE_FILE" ]; then
        antigravity_fragment_from_json < "$ANTIGRAVITY_CACHE_FILE"
    fi
    return 0
}

poll_antigravity() {
    local frag now_ts age
    frag=$(antigravity_fetch_fragment)
    now_ts=$(date +%s)
    if [ -n "$frag" ]; then
        ANTIGRAVITY_FRAGMENT="$frag"
        ANTIGRAVITY_LAST_FRAGMENT="$frag"
        ANTIGRAVITY_LAST_TS="$now_ts"
        return 0
    fi
    if [ -n "$ANTIGRAVITY_LAST_FRAGMENT" ]; then
        age=$(( now_ts - ANTIGRAVITY_LAST_TS ))
        if [ "$age" -lt "$ANTIGRAVITY_STALE_MAX" ]; then
            ANTIGRAVITY_FRAGMENT="$ANTIGRAVITY_LAST_FRAGMENT"
            log "Antigravity poll failed; reusing reading from ${age}s ago"
            return 0
        fi
        log "Antigravity unavailable for ${age}s — dropping the Antigravity tab data"
        ANTIGRAVITY_LAST_FRAGMENT=""
    fi
    ANTIGRAVITY_FRAGMENT=""
    return 0
}

# --- Usage statistics (the /stats screen) ---------------------------------
# Three sources, all read-only:
#   Claude — ~/.claude/stats-cache.json IS the file Claude Code's /stats reads.
#            We never recompute from the session JSONLs: slow, and it would drift
#            from what /stats reports. Claude Code owns and refreshes this file,
#            so the device mirrors /stats, staleness included.
#   Codex  — no equivalent cache; aggregate the rollout JSONLs (measured 0.15s
#            for 58 files, so no caching needed).
#   Antigravity — aggregate the CLI's transcript JSONLs and generation metadata
#                 SQLite databases (about 0.03s for 10 sessions on this machine).
# Stats ride every poll, like the usage bars. They used to be throttled to 300s
# on the assumption they move slowly — but the heatmap's newest cell is TODAY and
# it visibly lagged, so it tracks the usage cadence now. The scan behind it costs
# ~0.8s (mtime-filtered to today's files only), which fits inside a 30s poll.
STATS_INTERVAL=$POLL_INTERVAL
STATS_LAST_TS=0
DUNE_TOKENS=245000      # ~ the novel, for the "Nx more tokens than Dune" line
HEAT_DAYS=49            # 7x7 grid on the device

# Emit a stats payload for Claude, or nothing. Shape is documented in
# docs/superpowers/specs/2026-07-15-stats-screen-design.md.
stats_payload_claude() {
    local dir="${1:-$HOME/.claude}"
    [ -f "$dir/stats-cache.json" ] || return 0
    python3 - "$dir/stats-cache.json" "$DUNE_TOKENS" "$HEAT_DAYS" "$dir" <<'PYEOF' 2>/dev/null
import json, sys, datetime, glob, os

try:
    d = json.load(open(sys.argv[1]))
except Exception:
    sys.exit(0)
DUNE = int(sys.argv[2]); HEAT_DAYS = int(sys.argv[3])
CLAUDE_DIR = sys.argv[4]

# ---- today, live -----------------------------------------------------------
# Claude Code recomputes stats-cache.json on its own (slow) schedule, so its
# newest dailyActivity entry is usually YESTERDAY: the heatmap's last cell reads
# empty and the streak stops a day short, no matter how often we poll. Scan
# today's session transcripts ourselves and merge the result in.
# Only messageCount and tokens are merged — both verified comparable to the
# cache's own numbers (cache: 5644 msgs on the 14th, 11277 on the 12th; this scan
# finds 12630 today — same definition, same order). sessionCount is NOT merged:
# counting transcript files gives 241 where Claude Code reports 70, so its
# definition differs and merging it would report a number /stats never would.
def scan_today(claude_dir):
    today = datetime.date.today().isoformat()
    start = datetime.datetime.combine(datetime.date.today(), datetime.time.min).timestamp()
    msgs = 0; tok = 0
    try:
        files = [f for f in glob.glob(os.path.join(claude_dir, "projects", "**", "*.jsonl"),
                                      recursive=True)
                 if os.path.getmtime(f) >= start]
    except OSError:
        return 0, 0
    for f in files:
        try:
            fh = open(f, errors="replace")
        except OSError:
            continue
        with fh:
            for line in fh:
                # cheap prefilter before paying json.loads on a big transcript
                if today not in line or '"timestamp"' not in line:
                    continue
                try:
                    r = json.loads(line)
                except Exception:
                    continue          # truncated tail line — skip, never fatal
                if (r.get("timestamp") or "")[:10] != today:
                    continue
                t = r.get("type")
                if t in ("user", "assistant"):
                    msgs += 1
                if t == "assistant":
                    u = ((r.get("message") or {}).get("usage")) or {}
                    # same definition as the cache: non-cache input + output
                    tok += int(u.get("input_tokens", 0)) + int(u.get("output_tokens", 0))
    return msgs, tok

today_msgs, today_tok = scan_today(CLAUDE_DIR)
today_iso = datetime.date.today().isoformat()
if today_msgs > 0:
    da = [a for a in (d.get("dailyActivity") or []) if a.get("date") != today_iso]
    da.append({"date": today_iso, "messageCount": today_msgs})
    d["dailyActivity"] = da

mu = d.get("modelUsage") or {}
if not mu:
    sys.exit(0)

# /stats counts non-cache input + output only. cacheRead/cacheCreation are
# excluded — they dwarf everything (billions) and are not "tokens you used".
def total(v):
    return int(v.get("inputTokens", 0)) + int(v.get("outputTokens", 0))

tot = sum(total(v) for v in mu.values())
# Add today's tokens only when the cache hasn't already counted today, otherwise
# a freshly-recomputed cache would be double-counted.
if today_tok > 0 and (d.get("lastComputedDate") or "") < today_iso:
    tot += today_tok
if tot <= 0:
    sys.exit(0)
fav_raw = max(mu.items(), key=lambda kv: total(kv[1]))[0]

# claude-opus-4-8 -> "Opus 4.8"; leave unknown vendors' names alone.
def pretty(m):
    s = m
    if s.startswith("claude-"):
        s = s[len("claude-"):]
        s = s.split("-2")[0]                       # drop a trailing date stamp
        parts = s.split("-")
        if len(parts) >= 3 and parts[1].isdigit() and parts[2].isdigit():
            return "%s %s.%s" % (parts[0].capitalize(), parts[1], parts[2])
        if len(parts) >= 2 and parts[1].isdigit():
            return "%s %s" % (parts[0].capitalize(), parts[1])
        return parts[0].capitalize()
    return s[:15]

days = sorted({a["date"] for a in (d.get("dailyActivity") or [])
               if a.get("messageCount", 0) > 0})
if not days:
    sys.exit(0)
ds = [datetime.date.fromisoformat(x) for x in days]
today = datetime.date.today()
first = datetime.date.fromisoformat((d.get("firstSessionDate") or days[0])[:10])
span = (today - first).days + 1

best = cur = 1
for i in range(1, len(ds)):
    cur = cur + 1 if (ds[i] - ds[i-1]).days == 1 else 1
    best = max(best, cur)
run = 1
for i in range(len(ds) - 1, 0, -1):
    if (ds[i] - ds[i-1]).days == 1:
        run += 1
    else:
        break
# A streak only counts as "current" if it reaches today or yesterday.
streak = run if (today - ds[-1]).days <= 1 else 0

# Heatmap: one char per day, oldest->newest, quantised 0-4 by message count.
counts = {a["date"]: a.get("messageCount", 0) for a in (d.get("dailyActivity") or [])}
mx = max(counts.values()) if counts else 0
heat = []
for i in range(HEAT_DAYS - 1, -1, -1):
    c = counts.get((today - datetime.timedelta(days=i)).isoformat(), 0)
    heat.append("0" if c <= 0 or mx <= 0 else str(min(4, 1 + int(3 * c / mx))))

ls = int((d.get("longestSession") or {}).get("duration", 0) / 1000)
out = {
    "sv": 1, "p": "c",
    "tt": round(tot / 1e6, 1),
    "fm": pretty(fav_raw),
    "ns": int(d.get("totalSessions", 0)),
    "ls": ls,
    "ad": len(days), "as": max(span, len(days)),
    "cs": streak, "bs": best,
    "la": ds[-1].strftime("%b %-d"),
    "dn": int(round(tot / DUNE)),
    "hm": "".join(heat),
    }
sys.stdout.write(json.dumps(out, separators=(",", ":")))
PYEOF
}

# Emit a stats payload for Codex, or nothing.
stats_payload_codex() {
    local home="${CODEX_HOME:-$HOME/.codex}"
    [ -d "$home" ] || return 0
    python3 - "$home" "$DUNE_TOKENS" "$HEAT_DAYS" <<'PYEOF' 2>/dev/null
import json, sys, os, glob, datetime, collections

home = sys.argv[1]; DUNE = int(sys.argv[2]); HEAT_DAYS = int(sys.argv[3])
files = glob.glob(os.path.join(home, "sessions", "**", "rollout-*.jsonl"), recursive=True) \
      + glob.glob(os.path.join(home, "archived_sessions", "*.jsonl"))
if not files:
    sys.exit(0)

tot = 0; models = collections.Counter(); per_day = collections.Counter()
longest = 0; sessions = 0
for f in files:
    last = None; first_ts = None; last_ts = None; msgs = 0
    try:
        fh = open(f)
    except OSError:
        continue
    with fh:
        for line in fh:
            try:
                rec = json.loads(line)
            except Exception:
                continue    # a truncated tail line must not kill the whole file
            p = rec.get("payload") or {}
            ts = rec.get("timestamp")
            if ts:
                if first_ts is None or ts < first_ts: first_ts = ts
                if last_ts is None or ts > last_ts:   last_ts = ts
            t = p.get("type")
            if t == "token_count":
                u = (p.get("info") or {}).get("total_token_usage")
                if u:
                    last = u          # cumulative — the last one wins
                msgs += 1
            m = p.get("model") or (p.get("info") or {}).get("model")
            if m:
                models[m] += 1
    if last is None and first_ts is None:
        continue
    sessions += 1
    if last:
        # MUST subtract cached_input_tokens: Codex's input_tokens includes them,
        # Claude's /stats total does not. Without this the two tabs are not
        # comparable (218m vs 51m, of which 207m was re-sent cached context).
        tot += max(0, int(last.get("input_tokens", 0)) - int(last.get("cached_input_tokens", 0))) \
             + int(last.get("output_tokens", 0))
    if first_ts and last_ts:
        try:
            a = datetime.datetime.fromisoformat(first_ts.replace("Z", "+00:00"))
            b = datetime.datetime.fromisoformat(last_ts.replace("Z", "+00:00"))
            longest = max(longest, int((b - a).total_seconds()))
            per_day[a.astimezone().date().isoformat()] += max(1, msgs)
        except Exception:
            pass

if sessions == 0 or not per_day:
    sys.exit(0)

days = sorted(per_day)
ds = [datetime.date.fromisoformat(x) for x in days]
today = datetime.date.today()
span = (today - ds[0]).days + 1
best = cur = 1
for i in range(1, len(ds)):
    cur = cur + 1 if (ds[i] - ds[i-1]).days == 1 else 1
    best = max(best, cur)
run = 1
for i in range(len(ds) - 1, 0, -1):
    if (ds[i] - ds[i-1]).days == 1: run += 1
    else: break
streak = run if (today - ds[-1]).days <= 1 else 0

mx = max(per_day.values())
heat = []
for i in range(HEAT_DAYS - 1, -1, -1):
    c = per_day.get((today - datetime.timedelta(days=i)).isoformat(), 0)
    heat.append("0" if c <= 0 else str(min(4, 1 + int(3 * c / mx))))

fav = models.most_common(1)[0][0] if models else "codex"
out = {
    "sv": 1, "p": "x",
    "tt": round(tot / 1e6, 1),
    "fm": fav[:15],
    "ns": sessions,
    "ls": longest,
    "ad": len(days), "as": max(span, len(days)),
    "cs": streak, "bs": best,
    "la": ds[-1].strftime("%b %-d"),
    "dn": int(round(tot / DUNE)),
    "hm": "".join(heat),
    }
sys.stdout.write(json.dumps(out, separators=(",", ":")))
PYEOF
}

# Emit Antigravity CLI statistics from its read-only local transcripts. Token
# totals and favourite model come from the generation metadata protobuf stored
# by the CLI; session timing and activity come from its JSONL transcripts.
stats_payload_antigravity() {
    local home="${1:-${ANTIGRAVITY_HOME:-$HOME/.gemini/antigravity-cli}}"
    [ -d "$home" ] || return 0
    python3 - "$home" "$DUNE_TOKENS" "$HEAT_DAYS" <<'PYEOF' 2>/dev/null
import collections, datetime, glob, json, os, sqlite3, sys

home = sys.argv[1]; DUNE = int(sys.argv[2]); HEAT_DAYS = int(sys.argv[3])
transcripts = glob.glob(os.path.join(home, "brain", "*", ".system_generated", "logs", "transcript.jsonl"))
if not transcripts:
    sys.exit(0)

per_day = collections.Counter(); sessions = 0; longest = 0
for path in transcripts:
    first = last = None; model_events = 0
    try:
        fh = open(path, errors="replace")
    except OSError:
        continue
    with fh:
        for line in fh:
            try:
                rec = json.loads(line)
                ts = datetime.datetime.fromisoformat((rec.get("created_at") or "").replace("Z", "+00:00"))
            except Exception:
                continue
            first = ts if first is None or ts < first else first
            last = ts if last is None or ts > last else last
            if rec.get("source") == "MODEL":
                model_events += 1
                per_day[ts.astimezone().date().isoformat()] += 1
    if first is None:
        continue
    sessions += 1
    longest = max(longest, int(((last or first) - first).total_seconds()))
    if model_events == 0:
        per_day[first.astimezone().date().isoformat()] += 1

# Tiny protobuf wire reader: enough for agy's GenerationMetadata envelope.
def wire_fields(buf):
    out = []; i = 0
    def varint(pos):
        value = 0; shift = 0
        while pos < len(buf) and shift < 70:
            byte = buf[pos]; pos += 1; value |= (byte & 0x7f) << shift
            if byte < 0x80: return value, pos
            shift += 7
        raise ValueError
    while i < len(buf):
        try: key, i = varint(i)
        except Exception: break
        field, kind = key >> 3, key & 7
        try:
            if kind == 0: value, i = varint(i)
            elif kind == 1: value, i = buf[i:i+8], i + 8
            elif kind == 2:
                size, i = varint(i); value, i = buf[i:i+size], i + size
            elif kind == 5: value, i = buf[i:i+4], i + 4
            else: break
            out.append((field, kind, value))
        except Exception: break
    return out

def nested(fields, number):
    return [value for field, kind, value in fields if field == number and kind == 2]

total_tokens = 0; model_tokens = collections.Counter(); model_seen = collections.Counter()
for db_path in glob.glob(os.path.join(home, "conversations", "*.db")):
    try:
        conn = sqlite3.connect("file:%s?mode=ro" % db_path, uri=True)
        blobs = conn.execute("select data from gen_metadata where length(data)>0").fetchall()
        conn.close()
    except Exception:
        continue
    for (blob,) in blobs:
        for envelope in nested(wire_fields(blob), 1):
            fields = wire_fields(envelope)
            model = ""
            names = nested(fields, 21) or nested(fields, 19)
            if names:
                try: model = names[-1].decode("utf-8")
                except Exception: model = ""
            generation_tokens = 0
            for usage in nested(fields, 4):
                nums = {field: value for field, kind, value in wire_fields(usage) if kind == 0}
                generation_tokens += int(nums.get(1, 0)) + int(nums.get(3, 0))
            total_tokens += generation_tokens
            if model:
                model_seen[model] += 1
                model_tokens[model] += generation_tokens

if sessions == 0 or not per_day:
    sys.exit(0)
days = sorted(per_day); ds = [datetime.date.fromisoformat(day) for day in days]
today = datetime.date.today(); span = (today - ds[0]).days + 1
best = cur = 1
for i in range(1, len(ds)):
    cur = cur + 1 if (ds[i] - ds[i-1]).days == 1 else 1
    best = max(best, cur)
run = 1
for i in range(len(ds) - 1, 0, -1):
    if (ds[i] - ds[i-1]).days == 1: run += 1
    else: break
streak = run if (today - ds[-1]).days <= 1 else 0
mx = max(per_day.values())
heat = []
for i in range(HEAT_DAYS - 1, -1, -1):
    count = per_day.get((today - datetime.timedelta(days=i)).isoformat(), 0)
    heat.append("0" if count <= 0 else str(min(4, 1 + int(3 * count / mx))))

fav = (model_tokens.most_common(1) or model_seen.most_common(1) or [("Antigravity", 0)])[0][0]
fav = fav.replace(" (High)", "").replace("Gemini ", "Gem ")[:15]
out = {
    "sv": 1, "p": "g", "tt": round(total_tokens / 1e6, 1), "fm": fav,
    "ns": sessions, "ls": longest, "ad": len(days), "as": max(span, len(days)),
    "cs": streak, "bs": best, "la": ds[-1].strftime("%b %-d"),
    "dn": int(round(total_tokens / DUNE)), "hm": "".join(heat),
    }
sys.stdout.write(json.dumps(out, separators=(",", ":")))
PYEOF
}

# Send each provider's stats separately; combining them would exceed the
# firmware's 512-byte RX buffer. Any failure just skips that provider.
push_stats() {
    local now_ts payload
    now_ts=$(date +%s)
    [ $(( now_ts - STATS_LAST_TS )) -lt "$STATS_INTERVAL" ] && return 0
    STATS_LAST_TS="$now_ts"

    payload=$(stats_payload_claude "$PAYLOAD_DIR")
    if [ -n "$payload" ]; then
        log "Stats (claude): ${#payload} bytes"
        write_gatt "$RX_CHAR_PATH" "$payload" || log "Stats write failed (claude)"
        sleep 1   # let the firmware consume one payload before the next
    fi
    payload=$(stats_payload_codex)
    if [ -n "$payload" ]; then
        log "Stats (codex): ${#payload} bytes"
        write_gatt "$RX_CHAR_PATH" "$payload" || log "Stats write failed (codex)"
        sleep 1
    fi
    payload=$(stats_payload_antigravity)
    if [ -n "$payload" ]; then
        log "Stats (antigravity): ${#payload} bytes"
        write_gatt "$RX_CHAR_PATH" "$payload" || log "Stats write failed (antigravity)"
    fi
    return 0
}

# Extract the integer session % ("s") from a built payload, or 0.
_payload_session_pct() {
    echo "$1" | grep -o '"s":[0-9]*' | head -1 | cut -d: -f2
}

# Poll every configured config dir, decide which plan is "active", and send
# that plan's payload. "Active" = the plan whose session % rose most recently
# (recent API activity); a rise stamps LAST_ACTIVE so the choice is sticky and
# survives window resets (a drop to 0 isn't activity). Before any rise is seen
# (startup), fall back to the plan with the highest current session %.
poll() {
    POLL_SEQ=$((POLL_SEQ + 1))

    # Once per cycle, not once per Claude config dir: these providers are
    # machine-wide. Call directly (not via command substitution) so their cache
    # globals survive.
    poll_codex
    CODEX_CONTEXT_FRAGMENT=$(codex_context_fragment)
    poll_antigravity
    SYSTEM_FRAGMENT=$(system_resources_fragment)

    local -a dirs
    mapfile -t dirs < <(read_config_dirs)

    local -A cycle_payload cycle_s
    local dir token payload s
    for dir in "${dirs[@]}"; do
        token=$(read_token_for "$dir")
        if [ -z "$token" ]; then
            log "No token in $dir; skipping"
            continue
        fi
        PAYLOAD_DIR="$dir"   # which config dir this payload is for (plan label lookup)
        payload=$(build_payload_for_token "$token") || { log "API call failed for $dir"; continue; }
        [ -z "$payload" ] && continue
        s=$(_payload_session_pct "$payload"); s=${s:-0}
        cycle_payload["$dir"]="$payload"
        cycle_s["$dir"]="$s"
        # A rise in session % since the previous poll means this plan was just used.
        if [ -n "${PREV_S[$dir]:-}" ] && (( s > PREV_S[$dir] )); then
            LAST_ACTIVE["$dir"]=$POLL_SEQ
        fi
        PREV_S["$dir"]="$s"
    done

    if [ ${#cycle_payload[@]} -eq 0 ]; then
        log "No usable config dir this cycle"
        return 1
    fi

    # Pick the active dir: most recent activity wins; ties (and the no-activity
    # startup case) broken by highest current session %.
    local best_dir="" best_active=-1 best_s=-1 a
    for dir in "${!cycle_payload[@]}"; do
        a=${LAST_ACTIVE[$dir]:-0}
        s=${cycle_s[$dir]}
        if (( a > best_active )) || (( a == best_active && s > best_s )); then
            best_active=$a; best_s=$s; best_dir=$dir
        fi
    done

    if [ ${#dirs[@]} -gt 1 ]; then
        log "Active plan: $best_dir (s=$best_s)"
    fi
    log "Sending: ${cycle_payload[$best_dir]}"
    LAST_PAYLOAD="${cycle_payload[$best_dir]}"
    write_gatt "$RX_CHAR_PATH" "$LAST_PAYLOAD" || { log "Write failed"; return 1; }

    # Stats ride separately and far less often — see push_stats.
    PAYLOAD_DIR="$best_dir"
    push_stats
    return 0
}

cleanup() {
    stop_notify_subscriber
    log "Daemon stopped"
    exit 0
}

trap cleanup INT TERM

log "=== Claude Usage Tracker Daemon (BLE) ==="
log "Poll interval: ${POLL_INTERVAL}s"

BACKOFF=1

while true; do
    # Find the device: only a device the system already knows (paired/connected).
    # We never scan by name, so we can't grab a stranger's or the wrong nearby
    # unit. Pair the device once first (it's a bonded BLE HID keyboard anyway).
    if ! load_mac; then
        find_system_device_mac || {
            log "No paired/connected '$DEVICE_NAME'; waiting ${BACKOFF}s (not scanning)..."
            sleep "$BACKOFF"
            BACKOFF=$((BACKOFF < 60 ? BACKOFF * 2 : 60))
            continue
        }
    fi

    # Connect if not connected
    if ! is_connected; then
        connect_device || {
            log "Retrying in ${BACKOFF}s..."
            sleep "$BACKOFF"
            BACKOFF=$((BACKOFF < 60 ? BACKOFF * 2 : 60))
            continue
        }
    fi

    # Find the GATT characteristic
    RX_CHAR_PATH=$(find_char_path_by_uuid "$RX_CHAR_UUID")
    if [ -z "$RX_CHAR_PATH" ]; then
        log "Error: RX characteristic not found, retrying..."
        sleep 5
        continue
    fi
    log "GATT RX path: $RX_CHAR_PATH"

    BACKOFF=1  # reset backoff on successful connection

    start_notify_subscriber

    # Poll loop: tick every $TICK seconds. Poll Anthropic when the
    # interval has elapsed OR when the ESP requested a refresh. Between those
    # polls, re-send the last payload with fresh local system metrics so the
    # System screen stays live without hammering remote APIs.
    LAST_POLL=0
    while is_connected; do
        NOW=$(date +%s)
        if [ -f "$REFRESH_FLAG" ] || (( NOW - LAST_POLL >= POLL_INTERVAL )); then
            if [ -f "$REFRESH_FLAG" ]; then
                log "Refresh requested by device"
                rm -f "$REFRESH_FLAG"
            fi
            poll && LAST_POLL=$NOW
        elif [ -n "$LAST_PAYLOAD" ]; then
            refresh_cached_payload
        fi
        sleep "$TICK"
    done

    stop_notify_subscriber
    log "Device disconnected, reconnecting..."
    sleep 2
done
