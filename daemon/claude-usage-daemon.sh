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
POLL_INTERVAL=60
TICK=5
SAVED_MAC_FILE="$HOME/.config/claude-usage-monitor/ble-address"
CONFIG_FILE="$HOME/.config/claude-usage-monitor/config"
REFRESH_FLAG="/tmp/claude-usage-refresh-$$"
DBUS_DEST="org.bluez"
NOTIFY_PID=""

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
        payload=$(awk -v u5="$s5h_util" -v r5="$s5h_reset" -v u7="$s7d_util" -v r7="$s7d_reset" -v st="$s5h_status" -v now="$now" -v clk="$clock_fragment" -v chm="$chime_fragment" \
            'BEGIN {
                sp = sprintf("%.0f", u5 * 100);
                sr = (r5 - now) / 60; sr = sr > 0 ? sprintf("%.0f", sr) : 0;
                wp = sprintf("%.0f", u7 * 100);
                wr = (r7 - now) / 60; wr = wr > 0 ? sprintf("%.0f", wr) : 0;
                printf "{\"s\":%s,\"sr\":%s,\"w\":%s,\"wr\":%s,\"st\":\"%s\",\"acct\":\"pro\"%s%s,\"ok\":true}", sp, sr, wp, wr, st, clk, chm;
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
        payload=$(awk -v ou="$overage_util" -v or_="$overage_reset" -v st="$status" -v now="$now" -v pi="$period_info" -v clk="$clock_fragment" -v chm="$chime_fragment" \
            'BEGIN {
                sp = sprintf("%.0f", ou * 100);
                sr = (or_ - now) / 60; sr = sr > 0 ? sprintf("%.0f", sr) : 0;
                # Extract tp, pd, rd from period_info JSON (simple regex)
                tp = 0; pd = 30; rd = "";
                match(pi, /"tp": *([0-9]+)/, a); if (RSTART) tp = a[1];
                match(pi, /"pd": *([0-9]+)/, b); if (RSTART) pd = b[1];
                match(pi, /"rd": *"([^"]+)"/, c); if (RSTART) rd = c[1];
                printf "{\"s\":%s,\"sr\":%s,\"w\":0,\"wr\":0,\"st\":\"%s\",\"acct\":\"ent\",\"tp\":%s,\"pd\":%s,\"rd\":\"%s\"%s%s,\"ok\":true}", sp, sr, st, tp, pd, rd, clk, chm;
            }')
    fi

    printf '%s' "$payload"
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
    write_gatt "$RX_CHAR_PATH" "${cycle_payload[$best_dir]}" || { log "Write failed"; return 1; }
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
    # interval has elapsed OR when the ESP requested a refresh.
    LAST_POLL=0
    while is_connected; do
        NOW=$(date +%s)
        if [ -f "$REFRESH_FLAG" ] || (( NOW - LAST_POLL >= POLL_INTERVAL )); then
            if [ -f "$REFRESH_FLAG" ]; then
                log "Refresh requested by device"
                rm -f "$REFRESH_FLAG"
            fi
            poll && LAST_POLL=$NOW
        fi
        sleep "$TICK"
    done

    stop_notify_subscriber
    log "Device disconnected, reconnecting..."
    sleep 2
done
