#!/bin/bash
# Claude Usage Tracker Daemon (BLE)
# Reads Claude Code OAuth token, polls usage via the OAuth usage endpoint
# (api.anthropic.com/api/oauth/usage — same one `claude /usage` uses, costs
# zero tokens), sends results to the ESP32 over BLE GATT.
# Auto-connects and reconnects to the Claude Controller BLE device.
# Dependencies: curl, awk, python3, bluetoothctl

DEVICE_NAME="Claude Controller"
DEVICE_MAC="${DEVICE_MAC:-}"  # auto-discovered if empty
SERVICE_UUID="4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID="4c41555a-4465-7669-6365-000000000002"
REQ_CHAR_UUID="4c41555a-4465-7669-6365-000000000004"
POLL_INTERVAL=60
TICK=5
# Exponential backoff cap on consecutive poll failures (e.g. when the
# /api/oauth/usage endpoint returns 429). Backoff doubles from POLL_INTERVAL.
# 1h cap because the OAuth usage endpoint has aggressive rate limiting and
# the firmware surfaces the error state, so silent rapid retries add no value.
MAX_BACKOFF_INTERVAL=3600
SAVED_MAC_FILE="$HOME/.config/claude-usage-monitor/ble-address"
REFRESH_FLAG="/tmp/claude-usage-refresh-$$"
DBUS_DEST="org.bluez"
NOTIFY_PID=""

log() {
    echo "[$(date '+%H:%M:%S')] $1"
}

read_token() {
    grep -o '"accessToken":"[^"]*"' "$HOME/.claude/.credentials.json" | cut -d'"' -f4
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

# Scan for Claude Controller
scan_for_device() {
    log "Scanning for '$DEVICE_NAME'..."
    # Start LE scan
    bluetoothctl scan le &>/dev/null &
    local scan_pid=$!
    sleep 8
    kill "$scan_pid" 2>/dev/null
    wait "$scan_pid" 2>/dev/null

    # Pick the first matching device. Multiple matches happen when bluez
    # remembers old hardware (e.g. after swapping ESP boards). Stale entries
    # are removed on connect failure (see connect_device), so a few retry
    # cycles will converge on the live device.
    local found
    found=$(bluetoothctl devices 2>/dev/null | grep "$DEVICE_NAME" | head -1 | awk '{print $2}')
    if [ -n "$found" ]; then
        DEVICE_MAC="$found"
        save_mac
        log "Found: $DEVICE_MAC"
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
    if [ -f "$SAVED_MAC_FILE" ] && [ "$(cat "$SAVED_MAC_FILE")" = "$DEVICE_MAC" ]; then
        log "Invalidating cached MAC, will rescan by name"
        rm -f "$SAVED_MAC_FILE"
    fi
    # Remove from bluez so the next scan won't re-pick this dead MAC.
    # If the device comes back online it'll re-advertise and be re-discovered.
    bluetoothctl remove "$DEVICE_MAC" &>/dev/null
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

# Send a short error payload to the firmware so the device can surface the
# failure mode in place of the rotating spinner caption. Messages stay under
# 30 chars to fit the on-device label budget. Silently best-effort: a write
# failure here is logged in write_gatt but doesn't change poll's return code.
send_err() {
    local msg="$1"
    local payload
    payload=$(printf '{"ok":false,"err":"%s"}' "$msg")
    log "Sending error: $payload"
    write_gatt "$RX_CHAR_PATH" "$payload" >/dev/null 2>&1
}

poll() {
    local token
    token=$(read_token) || { log "Error: could not read token"; send_err "No auth token"; return 1; }

    # Capture both body and HTTP status. curl -w appends "\n<code>" so we
    # split on the trailing newline. curl's own exit code distinguishes a
    # network/DNS failure (non-zero) from an HTTP error (always exit 0 with
    # -s, no --fail).
    local response http_code body curl_exit
    response=$(curl -s -w $'\n%{http_code}' \
        "https://api.anthropic.com/api/oauth/usage" \
        -H "Authorization: Bearer $token" \
        -H "anthropic-beta: oauth-2025-04-20" \
        -H "Accept: application/json" \
        -H "User-Agent: claude-code/2.1.5" \
        2>/dev/null)
    curl_exit=$?
    if (( curl_exit != 0 )); then
        log "Error: curl exit $curl_exit (network failure)"
        send_err "No internet"
        return 1
    fi
    http_code="${response##*$'\n'}"
    body="${response%$'\n'*}"

    if (( http_code == 401 )); then
        log "API HTTP 401: $body"
        send_err "Auth expired"
        return 1
    elif (( http_code == 429 )); then
        log "API HTTP 429: $body"
        send_err "Rate limited"
        return 1
    elif (( http_code >= 500 )); then
        log "API HTTP $http_code: $body"
        send_err "Anthropic API down"
        return 1
    elif (( http_code != 200 )); then
        log "API HTTP $http_code: $body"
        send_err "API error $http_code"
        return 1
    fi

    local payload
    payload=$(python3 -c '
import datetime, json, sys, time

try:
    data = json.loads(sys.stdin.read())
except json.JSONDecodeError:
    sys.exit(1)

def pct(w):
    if not isinstance(w, dict):
        return 0
    u = w.get("utilization")
    return int(round(u)) if isinstance(u, (int, float)) else 0

def reset_mins(w):
    if not isinstance(w, dict):
        return -1
    s = w.get("resets_at")
    if not isinstance(s, str):
        return -1
    try:
        ts = datetime.datetime.fromisoformat(s.replace("Z", "+00:00")).timestamp()
    except ValueError:
        return -1
    m = (ts - time.time()) / 60.0
    return int(round(m)) if m > 0 else 0

fh = data.get("five_hour")
sd = data.get("seven_day")
s = pct(fh)
print(json.dumps({
    "s": s,
    "sr": reset_mins(fh),
    "w": pct(sd),
    "wr": reset_mins(sd),
    "st": "limited" if s >= 100 else "allowed",
    "ok": True,
}, separators=(",", ":")))
' <<< "$body") || { log "Error: failed to parse usage JSON"; send_err "Bad API response"; return 1; }

    log "Sending: $payload"
    write_gatt "$RX_CHAR_PATH" "$payload" || { log "Write failed"; return 1; }
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
    # Find the device
    if ! load_mac; then
        scan_for_device || {
            log "Device not found, retrying in ${BACKOFF}s..."
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
    FAILURES=0
    while is_connected; do
        NOW=$(date +%s)
        ELAPSED=$((NOW - LAST_POLL))
        # Exponential backoff on consecutive failures: 60s, 120s, 240s,
        # capped at MAX_BACKOFF_INTERVAL. Resets on success.
        EFFECTIVE=$((POLL_INTERVAL << FAILURES))
        (( EFFECTIVE > MAX_BACKOFF_INTERVAL )) && EFFECTIVE=$MAX_BACKOFF_INTERVAL

        PERIODIC_DUE=0
        (( ELAPSED >= EFFECTIVE )) && PERIODIC_DUE=1
        # Refresh bypasses POLL_INTERVAL during steady state, but respects
        # backoff — bypassing it on a rate-limited API would just keep
        # tripping the same 429.
        REFRESH_DUE=0
        if [ -f "$REFRESH_FLAG" ] && (( FAILURES == 0 )); then
            REFRESH_DUE=1
        fi

        if (( PERIODIC_DUE || REFRESH_DUE )); then
            if [ -f "$REFRESH_FLAG" ]; then
                log "Refresh requested by device"
                rm -f "$REFRESH_FLAG"
            fi
            if poll; then
                FAILURES=0
            else
                # Cap shift count to prevent integer overflow on long-running
                # failures; the value is already pinned to MAX_BACKOFF_INTERVAL.
                (( FAILURES < 10 )) && FAILURES=$((FAILURES + 1))
                NEXT=$((POLL_INTERVAL << FAILURES))
                (( NEXT > MAX_BACKOFF_INTERVAL )) && NEXT=$MAX_BACKOFF_INTERVAL
                log "Poll failed (#$FAILURES); next attempt in ${NEXT}s"
            fi
            LAST_POLL=$NOW
        fi
        sleep "$TICK"
    done

    stop_notify_subscriber
    log "Device disconnected, reconnecting..."
    sleep 2
done
