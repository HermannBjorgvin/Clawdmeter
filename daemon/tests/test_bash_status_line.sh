#!/bin/bash
# Regression test for the `status_line` option in claude-usage-daemon.sh:
# read_status_line_setting(), the "sl":0 field in build_payload_for_token(), and
# that the heartbeat replay keeps it. curl and write_gatt are stubbed.
set -u

DAEMON="$(dirname "$0")/../claude-usage-daemon.sh"

extract() { awk -v fn="$1" '$0 ~ "^"fn"\\(\\) \\{"{f=1} f{print} f&&/^\}/{exit}' "$DAEMON"; }
for fn in read_status_line_setting read_chime_setting read_clock_setting \
          detect_hour_format build_payload_for_token age_payload heartbeat; do
    eval "$(extract "$fn")"
done

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
CONFIG_FILE="$TMP/config"

fail=0
check() {  # check <label> <got> <want>
    if [ "$2" = "$3" ]; then echo "PASS: $1"; else echo "FAIL: $1: got '$2' want '$3'"; fail=1; fi
}
has_sl() { case "$1" in *'"sl":0'*) echo yes ;; *) echo no ;; esac; }

# Stub the API call: emit canned rate-limit headers for $ACCT (synthetic values).
NOW=$(date +%s)
curl() {
    cat >/dev/null  # swallow the -K - token config on stdin
    if [ "$ACCT" = "pro" ]; then
        printf 'HTTP/2 200\r\n'
        printf 'anthropic-ratelimit-unified-5h-utilization: 0.25\r\n'
        printf 'anthropic-ratelimit-unified-5h-reset: %s\r\n' $((NOW + 3600))
        printf 'anthropic-ratelimit-unified-7d-utilization: 0.5\r\n'
        printf 'anthropic-ratelimit-unified-7d-reset: %s\r\n' $((NOW + 86400))
        printf 'anthropic-ratelimit-unified-5h-status: allowed\r\n'
    else
        printf 'HTTP/2 200\r\n'
        printf 'anthropic-ratelimit-unified-overage-utilization: 0.1\r\n'
        printf 'anthropic-ratelimit-unified-overage-reset: %s\r\n' $((NOW + 7 * 86400))
        printf 'anthropic-ratelimit-unified-status: allowed\r\n'
    fi
}

# --- setting parser ---
rm -f "$CONFIG_FILE"
check "parse: no config -> on"             "$(read_status_line_setting)" "on"
printf 'chime = on\n' > "$CONFIG_FILE"
check "parse: key absent -> on"            "$(read_status_line_setting)" "on"
printf 'status_line = on\n' > "$CONFIG_FILE"
check "parse: on"                          "$(read_status_line_setting)" "on"
printf 'status_line = off\n' > "$CONFIG_FILE"
check "parse: off"                         "$(read_status_line_setting)" "off"
printf '  status_line=OFF  # hide\r\n' > "$CONFIG_FILE"
check "parse: whitespace, case, comment, CRLF" "$(read_status_line_setting)" "off"
printf 'status_line = sideways\n' > "$CONFIG_FILE"
check "parse: garbage -> on"               "$(read_status_line_setting)" "on"
printf 'status_line = off\nstatus_line = on\n' > "$CONFIG_FILE"
check "parse: last occurrence wins"        "$(read_status_line_setting)" "on"

# --- Pro payload ---
ACCT=pro
rm -f "$CONFIG_FILE"
p=$(build_payload_for_token "tok")
check "pro: unset -> no sl"                "$(has_sl "$p")" "no"
check "pro: unset payload shape unchanged" \
    "$(echo "$p" | sed -E 's/"sr":[0-9]+/"sr":N/; s/"wr":[0-9]+/"wr":N/')" \
    '{"s":25,"sr":N,"w":50,"wr":N,"st":"allowed","acct":"pro","ok":true}'
printf 'status_line = on\n' > "$CONFIG_FILE"
check "pro: on -> no sl"                   "$(has_sl "$(build_payload_for_token tok)")" "no"
printf 'status_line = maybe\n' > "$CONFIG_FILE"
check "pro: garbage -> no sl"              "$(has_sl "$(build_payload_for_token tok)")" "no"
printf 'status_line = off\n' > "$CONFIG_FILE"
p=$(build_payload_for_token "tok")
check "pro: off -> sl:0 before ok"         "$(echo "$p" | grep -o '"acct":"pro","sl":0,"ok":true}$')" '"acct":"pro","sl":0,"ok":true}'
check "pro: off payload is valid JSON"     "$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["sl"])' "$p")" "0"
printf 'chime = on\nstatus_line = off\n' > "$CONFIG_FILE"
check "pro: composes with chime"           "$(build_payload_for_token tok | grep -o '"c":1,"sl":0,"ok":true}$')" '"c":1,"sl":0,"ok":true}'

# --- Enterprise payload (its awk uses 3-arg match(), a gawk extension) ---
ACCT=ent
if awk 'BEGIN { match("a1", /([0-9])/, m) }' 2>/dev/null; then
    rm -f "$CONFIG_FILE"
    check "ent: unset -> no sl"            "$(has_sl "$(build_payload_for_token tok)")" "no"
    printf 'status_line = off\n' > "$CONFIG_FILE"
    p=$(build_payload_for_token "tok")
    check "ent: off -> sl:0"               "$(has_sl "$p")" "yes"
    check "ent: off payload is valid JSON" "$(python3 -c 'import json,sys; d=json.loads(sys.argv[1]); print(d["acct"], d["sl"])' "$p")" "ent 0"
else
    echo "SKIP: enterprise payload checks (this awk lacks gawk's 3-arg match())"
fi

# --- heartbeat replay keeps sl ---
WRITTEN="$TMP/written"
write_gatt() { printf '%s' "$2" > "$WRITTEN"; }
log() { :; }
RX_CHAR_PATH=/stub
LAST_PAYLOAD='{"s":25,"sr":60,"w":50,"wr":1440,"st":"allowed","acct":"pro","sl":0,"ok":true}'
LAST_PAYLOAD_TS=$(( $(date +%s) - 120 ))
heartbeat
check "heartbeat: replay keeps sl:0"       "$(has_sl "$(cat "$WRITTEN")")" "yes"
check "heartbeat: replay still ages sr"    "$(grep -o '"sr":[0-9]*' "$WRITTEN")" '"sr":58'

exit $fail
