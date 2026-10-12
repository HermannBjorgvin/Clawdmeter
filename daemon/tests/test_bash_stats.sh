#!/bin/bash
# Stats in claude-usage-daemon.sh: firmware_takes_stats() reads the firmware's
# TX value, send_stats() sends daemon/stats.py's messages once per change of
# Claude Code's stats cache.
set -u

DAEMON="$(dirname "$0")/../claude-usage-daemon.sh"

extract() { awk -v fn="$1" '$0 ~ "^"fn"\\(\\) \\{"{f=1} f{print} f&&/^\}/{exit}' "$DAEMON"; }
eval "$(extract firmware_takes_stats)"
eval "$(extract send_stats)"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
log() { :; }
write_gatt() { echo "$1 ${2:0:8}" >> "$TMP/writes"; }
find_char_path_by_uuid() { echo "/org/bluez/tx"; }
DBUS_DEST="org.bluez" TX_CHAR_UUID="tx"
STATS_PY="$(dirname "$0")/../stats.py"
STATS_CACHE="$TMP/stats-cache.json"
RX_CHAR_PATH="/org/bluez/rx" LAST_PAYLOAD='{"s":1}' STATS_SENT="" TAKES_STATS=1
echo '{"dailyActivity":[{"date":"2026-10-07","messageCount":3}],"totalSessions":1}' > "$STATS_CACHE"

fail=0
check() {  # check <label> <got> <want>
    if [ "$2" = "$3" ]; then echo "PASS: $1"; else echo "FAIL: $1: got '$2' want '$3'"; fail=1; fi
}
writes() { [ -f "$TMP/writes" ] && wc -l < "$TMP/writes" | tr -d ' ' || echo 0; }

busctl() { echo 'ay 22 123 34 97 99 107 34 58 116 114 117 101 44 34 115 116 97 116 115 34 58 49 125'; }  # {"ack":true,"stats":1}
firmware_takes_stats && got=yes || got=no
check "new firmware takes stats"   "$got" "yes"
busctl() { echo 'ay 12 123 34 97 99 107 34 58 116 114 117 101 125'; }                                   # {"ack":true}
firmware_takes_stats && got=yes || got=no
check "old firmware doesn't"       "$got" "no"

send_stats
check "sends both messages"        "$(writes)" "2"
check "to RX"                      "$(head -1 "$TMP/writes")" '/org/bluez/rx {"k":"hm'
send_stats
check "unchanged cache: no resend" "$(writes)" "2"
touch -t 203001010000 "$STATS_CACHE"
send_stats
check "changed cache: resend"      "$(writes)" "4"
TAKES_STATS=0 STATS_SENT=""
send_stats
check "old firmware: nothing sent" "$(writes)" "4"

exit $fail
