#!/bin/bash
# Regression test for the /stats payload builders in claude-usage-daemon.sh.
#
# The load-bearing rule here is TOKEN COMPARABILITY. Claude's stats-cache.json
# reports non-cache inputTokens+outputTokens (cacheRead/cacheCreation are
# excluded and are billions), while Codex's total_token_usage.input_tokens
# INCLUDES cached_input_tokens. Summing Codex raw gave 218.8m against Claude's
# 51.6m — a bogus 4x that was really re-sent cached context. Both sides must
# report non-cache input + output or the two tabs silently lie.
set -u

DAEMON="$(dirname "$0")/../claude-usage-daemon.sh"
eval "$(awk '/^stats_payload_claude\(\) \{/{f=1} f{print} f&&/^\}/{exit}' "$DAEMON")"
eval "$(awk '/^stats_payload_codex\(\) \{/{f=1} f{print} f&&/^\}/{exit}' "$DAEMON")"
DUNE_TOKENS=245000
HEAT_DAYS=49

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fails=0
check() { # name want got
    if [ "$2" = "$3" ]; then echo "PASS: $1"
    else echo "FAIL: $1"; echo "      expected [$2]"; echo "      got      [$3]"; fails=$((fails+1)); fi
}
field() { echo "$2" | python3 -c "
import json,sys
try: print(json.load(sys.stdin).get('$1', ''))
except Exception: print('')
"; }

TODAY=$(date +%Y-%m-%d)
D1=$(date -d "$TODAY -1 day" +%Y-%m-%d)
D2=$(date -d "$TODAY -2 day" +%Y-%m-%d)
D5=$(date -d "$TODAY -5 day" +%Y-%m-%d)

# --- Claude: token sum must EXCLUDE cache; favourite = highest in+out --------
mkdir -p "$TMP/c"
cat > "$TMP/c/stats-cache.json" <<JSON
{"version":4,"lastComputedDate":"$TODAY","totalSessions":42,
 "firstSessionDate":"${D5}T00:00:00.000Z",
 "longestSession":{"duration":857580939},
 "modelUsage":{
   "claude-opus-4-8":{"inputTokens":1000000,"outputTokens":2000000,
                      "cacheReadInputTokens":900000000,"cacheCreationInputTokens":50000000},
   "claude-haiku-4-5-20251001":{"inputTokens":10,"outputTokens":20,
                      "cacheReadInputTokens":0,"cacheCreationInputTokens":0}},
 "dailyActivity":[{"date":"$D5","messageCount":5},{"date":"$D2","messageCount":10},
                  {"date":"$D1","messageCount":20},{"date":"$TODAY","messageCount":40}]}
JSON
p=$(stats_payload_claude "$TMP/c")
check "claude: tokens exclude cacheRead/cacheCreation (3.0m not 953m)" "3.0" "$(field tt "$p")"
check "claude: favourite model prettified" "Opus 4.8" "$(field fm "$p")"
check "claude: sessions"  "42" "$(field ns "$p")"
check "claude: longest session seconds" "857580" "$(field ls "$p")"
check "claude: active days" "4" "$(field ad "$p")"
# D5 then a GAP then D2,D1,TODAY -> current streak 3, best 3
check "claude: gap breaks the streak (current)" "3" "$(field cs "$p")"
check "claude: best streak"                     "3" "$(field bs "$p")"
check "claude: dune ratio" "12" "$(field dn "$p")"
check "claude: heatmap is HEAT_DAYS chars" "49" "$(echo -n "$(field hm "$p")" | wc -c)"
check "claude: payload is a stats payload" "1" "$(field sv "$p")"
check "claude: provider tag" "c" "$(field p "$p")"

# newest day is the busiest -> last heat char must be the max level
hm=$(field hm "$p")
check "claude: busiest (newest) day is level 4" "4" "${hm: -1}"

# --- a stale last-active day must NOT count as a current streak --------------
cat > "$TMP/c/stats-cache.json" <<JSON
{"version":4,"totalSessions":1,"firstSessionDate":"${D5}T00:00:00.000Z",
 "longestSession":{"duration":1000},
 "modelUsage":{"claude-opus-4-8":{"inputTokens":1,"outputTokens":1,
               "cacheReadInputTokens":0,"cacheCreationInputTokens":0}},
 "dailyActivity":[{"date":"$D5","messageCount":5}]}
JSON
check "claude: streak is 0 when last activity is old" "0" "$(field cs "$(stats_payload_claude "$TMP/c")")"

# --- Claude degradation: every bad input yields NO payload ------------------
check "claude: missing cache file -> empty" "" "$(stats_payload_claude "$TMP/nope")"
echo 'garbage' > "$TMP/c/stats-cache.json"
check "claude: malformed cache -> empty" "" "$(stats_payload_claude "$TMP/c")"
echo '{}' > "$TMP/c/stats-cache.json"
check "claude: empty cache -> empty" "" "$(stats_payload_claude "$TMP/c")"
cat > "$TMP/c/stats-cache.json" <<JSON
{"modelUsage":{"m":{"inputTokens":0,"outputTokens":0,"cacheReadInputTokens":5,"cacheCreationInputTokens":5}},
 "dailyActivity":[{"date":"$TODAY","messageCount":1}]}
JSON
check "claude: zero non-cache tokens -> empty (not a 0.0m lie)" "" "$(stats_payload_claude "$TMP/c")"

# --- Codex: cached_input_tokens MUST be subtracted ---------------------------
export CODEX_HOME="$TMP/x"
mkdir -p "$CODEX_HOME/sessions/2026/07/15"
R="$CODEX_HOME/sessions/2026/07/15/rollout-a.jsonl"
{
  echo '{"timestamp":"'"$TODAY"'T01:00:00.000Z","type":"session_meta","payload":{"type":"session_meta","model":"gpt-5.6-sol"}}'
  # an early cumulative reading that must be superseded by the last one
  echo '{"timestamp":"'"$TODAY"'T01:01:00.000Z","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":50,"cached_input_tokens":10,"output_tokens":5}}}}'
  echo 'this line is corrupt and must be skipped, not fatal'
  echo '{"timestamp":"'"$TODAY"'T02:00:00.000Z","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":1000000,"cached_input_tokens":600000,"output_tokens":200000}}}}'
} > "$R"
p=$(stats_payload_codex)
# (1_000_000 - 600_000) + 200_000 = 600_000 -> 0.6m   (raw in+out would be 1.2m)
check "codex: cached input subtracted (0.6m not 1.2m)" "0.6" "$(field tt "$p")"
check "codex: last cumulative token_count wins" "0.6" "$(field tt "$p")"
check "codex: sessions" "1" "$(field ns "$p")"
check "codex: model from payload" "gpt-5.6-sol" "$(field fm "$p")"
check "codex: longest session seconds (1h)" "3600" "$(field ls "$p")"
check "codex: provider tag" "x" "$(field p "$p")"
check "codex: corrupt line skipped, payload still built" "1" "$(field sv "$p")"

# --- Codex degradation ------------------------------------------------------
export CODEX_HOME="$TMP/empty"; mkdir -p "$CODEX_HOME"
check "codex: no sessions -> empty" "" "$(stats_payload_codex)"
export CODEX_HOME="$TMP/gone"
check "codex: missing CODEX_HOME -> empty" "" "$(stats_payload_codex)"

echo
if [ "$fails" -eq 0 ]; then echo "All stats checks passed"; exit 0; fi
echo "$fails check(s) failed"
exit 1
