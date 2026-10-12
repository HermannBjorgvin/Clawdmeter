#!/bin/bash
# Attention alerts in claude-usage-daemon.sh: check_attention() consumes the
# hook files, attention_payload() adds b/m/x to a payload.
set -u

DAEMON="$(dirname "$0")/../claude-usage-daemon.sh"

extract() { awk -v fn="$1" '$0 ~ "^"fn"\\(\\) \\{"{f=1} f{print} f&&/^\}/{exit}' "$DAEMON"; }
eval "$(extract read_onoff_setting)"
eval "$(extract check_attention)"
eval "$(extract attention_payload)"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
CONFIG_FILE="$TMP/config"
ATTN_DIR="$TMP"
ALERT_SID="" ALERT_TS=0 ALERT_MSG="" ALERT_UNSENT=0 CLEAR_UNSENT=0

fail=0
check() {  # check <label> <got> <want>
    if [ "$2" = "$3" ]; then echo "PASS: $1"; else echo "FAIL: $1: got '$2' want '$3'"; fail=1; fi
}

printf '{"m":"Você  aprova\\na ação?","s":"abc"}' > "$TMP/attention"
check_attention
check "alert: message folded to ASCII"    "$ALERT_MSG" "Voce aprova a acao?"
check "alert: pending"                    "$ALERT_SID/$ALERT_UNSENT" "abc/1"
check "alert: file consumed"              "$([ -e "$TMP/attention" ] && echo yes || echo no)" "no"

check "payload: beep off by default"      "$(attention_payload '{"s":4}')" '{"s":4,"m":"Voce aprova a acao?"}'
echo 'beep = on' > "$CONFIG_FILE"
check "payload: beep on"                  "$(attention_payload '{"s":4}')" '{"s":4,"b":1,"m":"Voce aprova a acao?"}'
ALERT_UNSENT=0

printf '{"m":"Claude needs your permission","s":"abc"}' > "$TMP/attention"
check_attention
check "repeat: same session ignored"      "$ALERT_MSG/$ALERT_UNSENT" "Voce aprova a acao?/0"

touch -d '@'"$(( $(date +%s) + 2 ))" "$TMP/clear-other" 2>/dev/null || touch -t "$(date -v+2S +%Y%m%d%H%M.%S)" "$TMP/clear-other"
check_attention
check "clear: other session ignored"      "$ALERT_SID/$CLEAR_UNSENT" "abc/0"

touch -d '@'"$(( $(date +%s) + 2 ))" "$TMP/clear-abc" 2>/dev/null || touch -t "$(date -v+2S +%Y%m%d%H%M.%S)" "$TMP/clear-abc"
check_attention
check "clear: same session, newer"        "$ALERT_SID/$CLEAR_UNSENT" "/1"
check "payload: hide"                     "$(attention_payload '{"s":4}')" '{"s":4,"x":1}'

printf '{"m":"old","s":"zzz"}' > "$TMP/attention"
touch -d '@'"$(( $(date +%s) - 120 ))" "$TMP/attention" 2>/dev/null || touch -t "$(date -v-120S +%Y%m%d%H%M.%S)" "$TMP/attention"
CLEAR_UNSENT=0
check_attention
check "stale: ignored"                    "$ALERT_SID/$ALERT_UNSENT" "/0"

# mtimes in ns: the clear comes 0.5 s after the alert, inside the same second.
setmtime() { python3 -c 'import os,sys; os.utime(sys.argv[1], ns=(int(sys.argv[2]),)*2)' "$@"; }
sec=$(date +%s)
printf '{"m":"quick","s":"q"}' > "$TMP/attention"; setmtime "$TMP/attention" "${sec}100000000"
check_attention
touch "$TMP/clear-q"; setmtime "$TMP/clear-q" "${sec}600000000"
CLEAR_UNSENT=0 ALERT_UNSENT=0
check_attention
check "clear: same second, later"         "$ALERT_SID/$CLEAR_UNSENT" "/1"

exit $fail
