#!/bin/bash
# Claude Usage Tracker Daemon (USB CDC)
# Reads Claude Code OAuth token, polls usage via API, sends to ESP32 over
# the device's USB serial port (CDC). Replaces the older BLE GATT transport.
#
# Wire protocol matches firmware/src/serial_link.cpp:
#   host → device : {"s":..,"sr":..,"w":..,"wr":..,"st":"..","ok":..}\n
#   host → device : screenshot\n     (legacy framebuffer dump cmd)
#   device → host : ACK | NACK | REQ | READY | misc log lines
#
# Dependencies: curl, awk, stty

DEVICE_PORT="${DEVICE_PORT:-/dev/ttyACM0}"
BAUD=115200
POLL_INTERVAL=60
TICK=5
REFRESH_FLAG="/tmp/claude-usage-refresh-$$"
READER_PID=""

log() {
    echo "[$(date '+%H:%M:%S')] $1"
}

read_token() {
    grep -o '"accessToken":"[^"]*"' "$HOME/.claude/.credentials.json" | cut -d'"' -f4
}

# Configure the TTY:
#   raw / -echo                : no line discipline mangling
#   -hupcl                     : DON'T toggle DTR on close — the ESP32-S3
#                                resets on DTR transition with the default
#                                Arduino USB CDC config, which would reboot
#                                the firmware every time we open the port.
#   $BAUD                      : USB CDC ignores baud but stty needs it.
configure_port() {
    stty -F "$DEVICE_PORT" "$BAUD" raw -echo -echoe -echok -echoctl -echoke -hupcl 2>/dev/null
}

wait_for_port() {
    local backoff=1
    while [ ! -c "$DEVICE_PORT" ]; do
        log "Waiting for $DEVICE_PORT (retry in ${backoff}s)..."
        sleep "$backoff"
        backoff=$((backoff < 30 ? backoff * 2 : 30))
    done
}

# Background reader: tail incoming bytes from the device, watch for REQ
# refresh requests. Each REQ touches the flag file the inner loop polls.
# ACK/NACK lines are observable in the journal but otherwise ignored.
start_reader() {
    # cat blocks reading the tty; tagging each line makes the source obvious
    # in the journal. setsid puts cat into its own process group so we can
    # kill the whole pipeline atomically (the awk drops the flag and exits
    # cleanly on EOF if the device disappears).
    setsid bash -c "stdbuf -oL cat \"$DEVICE_PORT\" 2>/dev/null | stdbuf -oL awk -v flag='$REFRESH_FLAG' '
        {
            print \"[device] \" \$0
            if (\$0 ~ /^REQ\$/)   { system(\"touch \" flag) }
        }
        END { exit 0 }
    '" &
    READER_PID=$!
    log "Reader started (pgid=$READER_PID)"
}

stop_reader() {
    if [ -n "$READER_PID" ]; then
        kill -TERM -"$READER_PID" 2>/dev/null
        READER_PID=""
    fi
    rm -f "$REFRESH_FLAG"
}

# Returns 0 if the TTY exists and we can write to it.
port_alive() {
    [ -c "$DEVICE_PORT" ] && [ -w "$DEVICE_PORT" ]
}

# Send a single line. Returns nonzero on write failure (device unplugged).
write_line() {
    local line="$1"
    # printf appends '\n'. Redirect open with the noctty effect via exec
    # so we don't pay open/close cost per write — but a simple > re-open
    # works too because we set -hupcl. Keep it simple.
    printf '%s\n' "$line" > "$DEVICE_PORT" 2>/dev/null
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

    local payload
    payload=$(awk -v u5="$s5h_util" -v r5="$s5h_reset" -v u7="$s7d_util" -v r7="$s7d_reset" -v st="$status" -v now="$now" \
        'BEGIN {
            sp = sprintf("%.0f", u5 * 100);
            sr = (r5 - now) / 60; sr = sr > 0 ? sprintf("%.0f", sr) : 0;
            wp = sprintf("%.0f", u7 * 100);
            wr = (r7 - now) / 60; wr = wr > 0 ? sprintf("%.0f", wr) : 0;
            printf "{\"s\":%s,\"sr\":%s,\"w\":%s,\"wr\":%s,\"st\":\"%s\",\"ok\":true}", sp, sr, wp, wr, st;
        }')

    log "Sending: $payload"
    write_line "$payload" || { log "Write failed"; return 1; }
    return 0
}

cleanup() {
    stop_reader
    log "Daemon stopped"
    exit 0
}

trap cleanup INT TERM

log "=== Claude Usage Tracker Daemon (USB CDC) ==="
log "Port: $DEVICE_PORT"
log "Poll interval: ${POLL_INTERVAL}s"

while true; do
    wait_for_port
    configure_port

    log "Port opened: $DEVICE_PORT"
    start_reader

    # Tight inner loop: tick every $TICK seconds. Poll Anthropic when the
    # interval has elapsed, when the device asked for a refresh, or after
    # a write failure (which indicates the port has gone away).
    LAST_POLL=0
    WRITE_FAILS=0
    while port_alive; do
        NOW=$(date +%s)
        if [ -f "$REFRESH_FLAG" ] || (( NOW - LAST_POLL >= POLL_INTERVAL )); then
            if [ -f "$REFRESH_FLAG" ]; then
                log "Refresh requested by device"
                rm -f "$REFRESH_FLAG"
            fi
            if poll; then
                LAST_POLL=$NOW
                WRITE_FAILS=0
            else
                WRITE_FAILS=$((WRITE_FAILS + 1))
                if (( WRITE_FAILS >= 2 )); then
                    log "Write failed ${WRITE_FAILS}x, recycling port"
                    break
                fi
            fi
        fi
        sleep "$TICK"
    done

    stop_reader
    log "Port lost, reconnecting..."
    sleep 1
done
