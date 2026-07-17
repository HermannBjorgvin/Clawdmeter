#!/bin/bash
# Regression tests for the optional Antigravity CLI quota integration.
set -u

DAEMON="$(dirname "$0")/../claude-usage-daemon.sh"
eval "$(awk '/^find_antigravity_http_port\(\) \{/{f=1} f{print} f&&/^\}/{exit}' "$DAEMON")"
eval "$(awk '/^antigravity_fragment_from_json\(\) \{/{f=1} f{print} f&&/^\}/{exit}' "$DAEMON")"
eval "$(awk '/^antigravity_fetch_fragment\(\) \{/{f=1} f{print} f&&/^\}/{exit}' "$DAEMON")"
eval "$(awk '/^poll_antigravity\(\) \{/{f=1} f{print} f&&/^\}/{exit}' "$DAEMON")"
eval "$(awk '/^stats_payload_antigravity\(\) \{/{f=1} f{print} f&&/^\}/{exit}' "$DAEMON")"
DUNE_TOKENS=245000
HEAT_DAYS=49

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
ANTIGRAVITY_CACHE_FILE="$TMP/antigravity-quota.json"

fails=0
check() {
    if [ "$2" = "$3" ]; then echo "PASS: $1"
    else echo "FAIL: $1"; echo "      expected [$2]"; echo "      got      [$3]"; fails=$((fails+1)); fi
}
field() { echo "$2" | python3 -c "import json,sys; print(json.load(sys.stdin).get('$1', ''))" 2>/dev/null; }

ANTIGRAVITY_HTTP_PORT=45678
ANTIGRAVITY_NOW=1784131200
FAKE_JSON=''
FAKE_RC=0
curl() { [ "$FAKE_RC" -ne 0 ] && return "$FAKE_RC"; printf '%s' "$FAKE_JSON"; }

FAKE_JSON='{"response":{"groups":[
 {"displayName":"Gemini Models","buckets":[
  {"window":"weekly","remainingFraction":0.75,"resetTime":"2026-07-22T16:00:00Z"},
  {"window":"5h","remainingFraction":0.4,"resetTime":"2026-07-15T21:00:00Z"}]},
 {"displayName":"Claude and GPT models","buckets":[
  {"window":"weekly","remainingFraction":0.01,"resetTime":"2026-07-20T00:00:00Z"},
  {"window":"5h","remainingFraction":0.02,"resetTime":"2026-07-16T01:00:00Z"}]}
]}}'
frag="$(antigravity_fetch_fragment)"
case "$frag" in
    *'"ag5":60'*'"agw":25'*'"agpl":"Gemini Models"'*)
        echo "PASS: Gemini pool becomes current + weekly used percentages" ;;
    *) echo "FAIL: wrong Antigravity fragment [$frag]"; fails=$((fails+1)) ;;
esac
case "$frag" in
    *'"ag5":98'*|*'"agw":99'*) echo "FAIL: third-party pool leaked into Gemini tab"; fails=$((fails+1)) ;;
    *) echo "PASS: Claude/GPT pool is not merged into Gemini quotas" ;;
esac

rm -f "$ANTIGRAVITY_CACHE_FILE"
FAKE_JSON='not-json'
check "malformed response -> empty" "" "$(antigravity_fetch_fragment)"
rm -f "$ANTIGRAVITY_CACHE_FILE"
FAKE_JSON='{"response":{"groups":[]}}'
check "missing Gemini group -> empty" "" "$(antigravity_fetch_fragment)"
rm -f "$ANTIGRAVITY_CACHE_FILE"
FAKE_RC=7
check "local server unavailable -> empty" "" "$(antigravity_fetch_fragment)"
FAKE_RC=0

ANTIGRAVITY_FRAGMENT=""; ANTIGRAVITY_LAST_FRAGMENT=""; ANTIGRAVITY_LAST_TS=0
ANTIGRAVITY_STALE_MAX=600
log() { :; }
FAKE_JSON='{"response":{"groups":[{"displayName":"Gemini Models","buckets":[
 {"window":"weekly","remainingFraction":0.8,"resetTime":"2026-07-22T16:00:00Z"},
 {"window":"5h","remainingFraction":0.3,"resetTime":"2026-07-15T21:00:00Z"}]}]}}'
poll_antigravity
case "$ANTIGRAVITY_FRAGMENT" in *'"ag5":70'*) echo "PASS: good poll populates cache";; *) echo "FAIL: good poll [$ANTIGRAVITY_FRAGMENT]"; fails=$((fails+1));; esac
FAKE_RC=28
poll_antigravity
case "$ANTIGRAVITY_FRAGMENT" in *'"ag5":70'*) echo "PASS: transient failure reuses cache";; *) echo "FAIL: cache was not reused"; fails=$((fails+1));; esac

# Closing agy must not make an authenticated user appear signed out. The raw
# response survives daemon restarts, and relative reset times are recomputed
# from its absolute timestamps rather than freezing at the last live value.
ANTIGRAVITY_FRAGMENT=""; ANTIGRAVITY_LAST_FRAGMENT=""; ANTIGRAVITY_LAST_TS=0
ANTIGRAVITY_NOW=$((1784131200 + 120))
poll_antigravity
case "$ANTIGRAVITY_FRAGMENT" in
    *'"ag5":70'*'"ag5r":298'*) echo "PASS: closed agy uses persistent quota with ticking reset";;
    *) echo "FAIL: persistent quota not reused [$ANTIGRAVITY_FRAGMENT]"; fails=$((fails+1));;
esac

# Stats are read from local Antigravity transcripts even when the CLI is closed.
AGHOME="$TMP/ag"
mkdir -p "$AGHOME/brain/a/.system_generated/logs" "$AGHOME/brain/b/.system_generated/logs"
TODAY=$(date +%Y-%m-%d)
YDAY=$(date -d "$TODAY -1 day" +%Y-%m-%d)
cat > "$AGHOME/brain/a/.system_generated/logs/transcript.jsonl" <<JSON
{"created_at":"${YDAY}T01:00:00Z","source":"USER","type":"USER_INPUT"}
{"created_at":"${YDAY}T01:30:00Z","source":"MODEL","type":"PLANNER_RESPONSE"}
JSON
cat > "$AGHOME/brain/b/.system_generated/logs/transcript.jsonl" <<JSON
{"created_at":"${TODAY}T02:00:00Z","source":"USER","type":"USER_INPUT"}
{"created_at":"${TODAY}T03:00:00Z","source":"MODEL","type":"PLANNER_RESPONSE"}
JSON
p="$(stats_payload_antigravity "$AGHOME")"
check "stats provider tag" "g" "$(field p "$p")"
check "stats session count" "2" "$(field ns "$p")"
check "stats active days" "2" "$(field ad "$p")"
check "stats longest session" "3600" "$(field ls "$p")"
check "stats heatmap length" "49" "$(echo -n "$(field hm "$p")" | wc -c)"

echo
if [ "$fails" -eq 0 ]; then echo "All Antigravity daemon checks passed"; exit 0; fi
echo "$fails check(s) failed"
exit 1
