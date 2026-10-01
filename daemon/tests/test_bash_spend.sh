#!/bin/bash
# Regression test for the Enterprise spend helpers in claude-usage-daemon.sh:
# read_spend_setting(), parse_spend_json(), format_spend(), splice_spend() and
# update_spend_for().
#
# /api/oauth/usage was adopted once and reverted after a 429 retry loop, so
# update_spend_for() must hit it at most once per SPEND_CACHE_TTL per dir and
# not at all during the 429 cooldown. curl is stubbed; no network is used.
set -u

DAEMON="$(dirname "$0")/../claude-usage-daemon.sh"

extract() { awk -v fn="$1" '$0 ~ "^"fn"\\(\\) \\{"{f=1} f{print} f&&/^\}/{exit}' "$DAEMON"; }
eval "$(extract read_spend_setting)"
eval "$(extract parse_spend_json)"
eval "$(extract format_spend)"
eval "$(extract splice_spend)"
eval "$(extract update_spend_for)"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
CONFIG_FILE="$TMP/config"
USAGE_URL="https://example.invalid/api/oauth/usage"
SPEND_CACHE_TTL=300
SPEND_COOLDOWN=900
declare -A SPEND_FIELDS SPEND_TRIED_TS
SPEND_COOLDOWN_UNTIL=0
log() { echo "$1" >> "$TMP/log"; }

fail=0
check() {  # check <label> <got> <want>
    if [ "$2" = "$3" ]; then echo "PASS: $1"; else echo "FAIL: $1: got '$2' want '$3'"; fail=1; fi
}

SAMPLE='{"five_hour":null,"seven_day":null,"extra_usage":{"is_enabled":true,"monthly_limit":50000,"used_credits":1250.0,"utilization":2.5,"currency":"USD","decimal_places":2},"spend":{"used":{"amount_minor":1250,"currency":"USD","exponent":2},"limit":{"amount_minor":50000,"currency":"USD","exponent":2},"percent":2,"enabled":true}}'
FRAG=',"mu":1250,"ml":50000,"mx":2,"cur":"USD"'

# --- setting ---
rm -f "$CONFIG_FILE"
check "setting: no config -> off"         "$(read_spend_setting)" "off"
printf 'spend = ON  # comment\r\n' > "$CONFIG_FILE"
check "setting: case, comment, CRLF"      "$(read_spend_setting)" "on"
printf 'spend = yes\n' > "$CONFIG_FILE"
check "setting: garbage -> off"           "$(read_spend_setting)" "off"

# --- parsing ---
check "parse: spend block"                "$(parse_spend_json "$SAMPLE")" "$FRAG"
check "parse: extra_usage fallback" \
    "$(parse_spend_json '{"extra_usage":{"monthly_limit":50000,"used_credits":1234.0,"currency":"eur","decimal_places":2}}')" \
    ',"mu":1234,"ml":50000,"mx":2,"cur":"EUR"'
check "parse: bad spend falls back to extra_usage" \
    "$(parse_spend_json '{"spend":{"used":{"amount_minor":"x"},"limit":{}},"extra_usage":{"monthly_limit":100,"used_credits":5,"currency":"USD","decimal_places":2}}')" \
    ',"mu":5,"ml":100,"mx":2,"cur":"USD"'
check "parse: limit 0 -> nothing" \
    "$(parse_spend_json '{"spend":{"used":{"amount_minor":1,"currency":"USD","exponent":2},"limit":{"amount_minor":0,"currency":"USD","exponent":2}}}')" ""
check "parse: missing currency -> nothing" \
    "$(parse_spend_json '{"extra_usage":{"monthly_limit":100,"used_credits":5,"decimal_places":2}}')" ""
check "parse: not JSON -> nothing"        "$(parse_spend_json 'not json')" ""
check "parse: null blocks -> nothing"     "$(parse_spend_json '{"spend":null,"extra_usage":null}')" ""

# --- formatting / splicing ---
check "format: USD"                       "$(format_spend "$FRAG")" '$12.50/$500.00'
check "format: non-USD"                   "$(format_spend ',"mu":1500,"ml":100000,"mx":0,"cur":"JPY"')" 'JPY 1500/JPY 100000'
ENT='{"s":2,"sr":100,"w":0,"wr":0,"st":"allowed","acct":"ent","tp":3,"pd":30,"rd":"Oct 31","ok":true}'
check "splice: before ok" "$(splice_spend "$ENT" "$FRAG")" \
    '{"s":2,"sr":100,"w":0,"wr":0,"st":"allowed","acct":"ent","tp":3,"pd":30,"rd":"Oct 31","mu":1250,"ml":50000,"mx":2,"cur":"USD","ok":true}'
check "splice: empty fragment is a no-op" "$(splice_spend "$ENT" "")" "$ENT"
check "splice: unexpected tail untouched" "$(splice_spend '{"ok":false}' "$FRAG")" '{"ok":false}'

# --- fetch: cache, 429 cooldown, failures (curl stubbed) ---
CALLS="$TMP/calls"
: > "$CALLS"
CURL_BODY="$SAMPLE"; CURL_CODE=200
curl() { echo x >> "$CALLS"; cat >/dev/null; printf '%s\n%s' "$CURL_BODY" "$CURL_CODE"; }
ncalls() { wc -l < "$CALLS" | tr -d ' '; }

update_spend_for /d tok
check "fetch: first call hits endpoint"   "$(ncalls)" "1"
check "fetch: fragment cached"            "${SPEND_FIELDS[/d]:-}" "$FRAG"
update_spend_for /d tok
check "fetch: second call within TTL is cached" "$(ncalls)" "1"

SPEND_TRIED_TS[/d]=$(( $(date +%s) - 301 )); CURL_CODE=401; CURL_BODY='{"error":"x"}'
update_spend_for /d tok; rc=$?
check "fetch: 401 returns success"        "$rc" "0"
check "fetch: 401 keeps last good fields" "${SPEND_FIELDS[/d]:-}" "$FRAG"
check "fetch: 401 made one request"       "$(ncalls)" "2"
update_spend_for /d tok
check "fetch: no retry right after 401"   "$(ncalls)" "2"

SPEND_TRIED_TS[/d]=$(( $(date +%s) - 301 )); CURL_CODE=429
update_spend_for /d tok
check "fetch: 429 made one request"       "$(ncalls)" "3"
check "fetch: 429 sets cooldown"          "$(( SPEND_COOLDOWN_UNTIL > $(date +%s) + 890 ))" "1"
SPEND_TRIED_TS[/d]=0; SPEND_TRIED_TS[/other]=0
update_spend_for /d tok; update_spend_for /other tok2
check "fetch: no requests during cooldown (any dir)" "$(ncalls)" "3"
check "fetch: cooldown keeps last good fields" "${SPEND_FIELDS[/d]:-}" "$FRAG"

SPEND_COOLDOWN_UNTIL=0; CURL_CODE=200; CURL_BODY='garbage'
update_spend_for /fresh tok3
check "fetch: bad body, no cache -> no fields" "${SPEND_FIELDS[/fresh]:-}" ""

exit $fail
