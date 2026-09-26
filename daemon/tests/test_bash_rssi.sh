#!/bin/bash
# Regression test for read_rssi_setting() in claude-usage-daemon.sh: the
# `rssi` key that opts in to the BLE signal bars ("rb":1). Off by default.
set -u

DAEMON="$(dirname "$0")/../claude-usage-daemon.sh"

extract() { awk -v fn="$1" '$0 ~ "^"fn"\\(\\) \\{"{f=1} f{print} f&&/^\}/{exit}' "$DAEMON"; }
eval "$(extract read_rssi_setting)"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
CONFIG_FILE="$TMP/config"

fail=0
check() {  # check <label> <got> <want>
    if [ "$2" = "$3" ]; then echo "PASS: $1"; else echo "FAIL: $1: got '$2' want '$3'"; fail=1; fi
}

rm -f "$CONFIG_FILE"
check "no config -> off"                       "$(read_rssi_setting)" "off"
printf 'clock = auto\nchime = on\n' > "$CONFIG_FILE"
check "key absent -> off"                      "$(read_rssi_setting)" "off"
printf 'rssi = on\n' > "$CONFIG_FILE"
check "on"                                     "$(read_rssi_setting)" "on"
printf '  rssi = ON   # bars\r\n' > "$CONFIG_FILE"
check "value case, whitespace, comment, CRLF"  "$(read_rssi_setting)" "on"
printf 'rssi = yes\n' > "$CONFIG_FILE"
check "junk -> off"                            "$(read_rssi_setting)" "off"
printf 'rssi = on\nrssi = off\n' > "$CONFIG_FILE"
check "last occurrence wins"                   "$(read_rssi_setting)" "off"

exit $fail
