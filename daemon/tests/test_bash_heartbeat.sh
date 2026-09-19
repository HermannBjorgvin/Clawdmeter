#!/bin/bash
# Regression test for the poll/heartbeat helpers in claude-usage-daemon.sh:
# read_poll_interval(), read_heartbeat_interval() and age_payload().
#
# The firmware marks data stale 90s after the last payload and only redraws
# its "resets in" countdown when a payload lands, so when poll_interval is
# raised above ~80s the heartbeat replay (with aged sr/wr/t) is the only thing
# keeping the display live. These helpers must parse the config leniently and
# age the payload without touching anything else.
set -u

DAEMON="$(dirname "$0")/../claude-usage-daemon.sh"

extract() { awk -v fn="$1" '$0 ~ "^"fn"\\(\\) \\{"{f=1} f{print} f&&/^\}/{exit}' "$DAEMON"; }
eval "$(extract read_poll_interval)"
eval "$(extract read_heartbeat_interval)"
eval "$(extract age_payload)"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
CONFIG_FILE="$TMP/config"
POLL_INTERVAL_DEFAULT=60
HEARTBEAT_INTERVAL_DEFAULT=60

fail=0
check() {  # check <label> <got> <want>
    if [ "$2" = "$3" ]; then echo "PASS: $1"; else echo "FAIL: $1: got '$2' want '$3'"; fail=1; fi
}

# --- interval parsing ---
rm -f "$CONFIG_FILE"
check "poll: no config -> default"        "$(read_poll_interval)" "60"
check "heartbeat: no config -> default"   "$(read_heartbeat_interval)" "60"
printf 'poll_interval = 300\nheartbeat_interval = 45\n' > "$CONFIG_FILE"
check "poll: plain value"                 "$(read_poll_interval)" "300"
check "heartbeat: plain value"            "$(read_heartbeat_interval)" "45"
printf '  poll_interval=120 # comment\r\n' > "$CONFIG_FILE"
check "poll: whitespace, comment, CRLF"   "$(read_poll_interval)" "120"
printf 'poll_interval = 3\nheartbeat_interval = 0\n' > "$CONFIG_FILE"
check "poll: clamped to 10"               "$(read_poll_interval)" "10"
check "heartbeat: clamped to 10"          "$(read_heartbeat_interval)" "10"
printf 'poll_interval = abc\n' > "$CONFIG_FILE"
check "poll: garbage -> default"          "$(read_poll_interval)" "60"
printf 'poll_interval = 30\npoll_interval = 90\n' > "$CONFIG_FILE"
check "poll: last occurrence wins"        "$(read_poll_interval)" "90"
POLL_INTERVAL_DEFAULT=45; rm -f "$CONFIG_FILE"
check "poll: env-provided default"        "$(read_poll_interval)" "45"

# --- payload aging ---
check "age: 300s ticks sr/wr down 5 min" \
    "$(age_payload '{"s":13,"sr":73,"w":21,"wr":6913,"st":"allowed","acct":"pro","ok":true}' 300)" \
    '{"s":13,"sr":68,"w":21,"wr":6908,"st":"allowed","acct":"pro","ok":true}'
check "age: sub-minute elapsed is a no-op on sr/wr" \
    "$(age_payload '{"s":13,"sr":73,"w":21,"wr":6913,"st":"allowed","ok":true}' 59)" \
    '{"s":13,"sr":73,"w":21,"wr":6913,"st":"allowed","ok":true}'
check "age: clamps at 0, advances clock t by seconds, keeps chime flag and tf" \
    "$(age_payload '{"s":13,"sr":2,"w":21,"wr":0,"st":"allowed","c":1,"t":1758300000,"tf":12,"ok":true}' 245)" \
    '{"s":13,"sr":0,"w":21,"wr":0,"st":"allowed","c":1,"t":1758300245,"tf":12,"ok":true}'
check "age: enterprise payload untouched apart from sr" \
    "$(age_payload '{"s":99.5,"sr":61,"w":0,"wr":0,"st":"allowed","acct":"ent","tp":40,"pd":30,"rd":"Oct 1","ok":true}' 60)" \
    '{"s":99.5,"sr":60,"w":0,"wr":0,"st":"allowed","acct":"ent","tp":40,"pd":30,"rd":"Oct 1","ok":true}'

exit $fail
