#!/bin/bash
# Claude Usage Tracker Daemon
# Reads Claude Code OAuth token, polls usage via API, sends to ESP32 over serial.
# Survives USB plug/unplug cycles using inotifywait for instant device detection.
# Dependencies: curl, awk, inotify-tools

SERIAL_PORT="${SERIAL_PORT:-/dev/ttyACM0}"
SERIAL_DEV=$(basename "$SERIAL_PORT")
POLL_INTERVAL=30
BAUD=115200
SERIAL_FD=3

log() {
    echo "[$(date '+%H:%M:%S')] $1"
}

read_token() {
    grep -o '"accessToken":"[^"]*"' "$HOME/.claude/.credentials.json" | cut -d'"' -f4
}

device_connected() {
    [ -e "$SERIAL_PORT" ]
}

open_serial() {
    stty -F "$SERIAL_PORT" "$BAUD" raw -echo -echoe -echok 2>/dev/null || return 1
    eval "exec ${SERIAL_FD}<>\"$SERIAL_PORT\"" 2>/dev/null || return 1
    return 0
}

close_serial() {
    eval "exec ${SERIAL_FD}>&-" 2>/dev/null
}

# Wait for device to appear using inotifywait (blocks, zero CPU)
wait_for_device() {
    while ! device_connected; do
        inotifywait -qq -e create /dev --include "$SERIAL_DEV" 2>/dev/null || sleep 1
    done
}

poll() {
    local token
    token=$(read_token) || { log "Error: could not read token"; return 1; }
    local now
    now=$(date +%s)

    local headers
    headers=$(curl -s -D - -o /dev/null \
        "https://api.anthropic.com/v1/messages" \
        -H "Authorization: Bearer $token" \
        -H "anthropic-version: 2023-06-01" \
        -H "anthropic-beta: oauth-2025-04-20" \
        -H "Content-Type: application/json" \
        -H "User-Agent: claude-code/2.1.5" \
        -d '{"model":"claude-haiku-4-5-20251001","max_tokens":1,"messages":[{"role":"user","content":"hi"}]}' \
        2>/dev/null) || { log "Error: API call failed"; return 1; }

    local s5h_util s5h_reset s7d_util s7d_reset status
    s5h_util=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-5h-utilization" | tr -d '\r' | awk '{print $2}')
    s5h_reset=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-5h-reset" | tr -d '\r' | awk '{print $2}')
    s7d_util=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-7d-utilization" | tr -d '\r' | awk '{print $2}')
    s7d_reset=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-7d-reset" | tr -d '\r' | awk '{print $2}')
    status=$(echo "$headers" | grep -i "anthropic-ratelimit-unified-5h-status" | tr -d '\r' | awk '{print $2}')

    s5h_util=${s5h_util:-0}
    s5h_reset=${s5h_reset:-0}
    s7d_util=${s7d_util:-0}
    s7d_reset=${s7d_reset:-0}
    status=${status:-unknown}

    # Build JSON payload in a single awk process (utilization * 100, timestamps to minutes)
    local payload
    payload=$(awk -v u5="$s5h_util" -v r5="$s5h_reset" -v u7="$s7d_util" -v r7="$s7d_reset" -v st="$status" -v now="$now" \
        'BEGIN {
            sp = sprintf("%.0f", u5 * 100);
            sr = (r5 - now) / 60; sr = sr > 0 ? sprintf("%.0f", sr) : 0;
            wp = sprintf("%.0f", u7 * 100);
            wr = (r7 - now) / 60; wr = wr > 0 ? sprintf("%.0f", wr) : 0;
            printf "{\"s\":%s,\"sr\":%s,\"w\":%s,\"wr\":%s,\"st\":\"%s\",\"ok\":true}", sp, sr, wp, wr, st;
        }')

    if ! echo "$payload" >&${SERIAL_FD} 2>/dev/null; then
        log "Write failed - device disconnected"
        return 1
    fi
}

cleanup() {
    close_serial
    log "Daemon stopped"
    exit 0
}

trap cleanup INT TERM

log "=== Claude Usage Tracker Daemon ==="
log "Serial port: $SERIAL_PORT"
log "Poll interval: ${POLL_INTERVAL}s"

while true; do
    # Wait for device (instant detection via inotify, or already plugged in)
    if ! device_connected; then
        log "Waiting for device..."
        wait_for_device
    fi

    log "Device connected"
    sleep 2  # wait for ESP32 boot

    if ! open_serial; then
        log "Failed to open serial port"
        sleep 2
        continue
    fi
    log "Serial port opened"

    # Poll loop - polls API every POLL_INTERVAL, exits on disconnect or write failure
    while device_connected; do
        poll || break
        # Sleep POLL_INTERVAL but wake instantly if device disappears
        inotifywait -qq -e delete /dev --include "$SERIAL_DEV" -t "$POLL_INTERVAL" 2>/dev/null
    done

    # Disconnected - clean up and loop back to wait
    log "Device disconnected"
    close_serial
done
