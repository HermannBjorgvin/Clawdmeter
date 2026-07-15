#!/bin/bash
# Regression test for poll_codex() / read_codex_token() in claude-usage-daemon.sh.
#
# Codex is strictly optional: every failure path must yield an EMPTY fragment so
# the firmware falls back to the Claude-only 2-bar view. A non-empty fragment on
# a failure would render a bogus Codex bar; a crash would take the Claude payload
# down with it (symptom: device goes stale despite a healthy Anthropic poll).
#
# Also pins window selection. Plus plans expose only primary_window (7d) with
# secondary_window null — the "primary = 5h" folk wisdom is wrong — so the
# selection must be driven by used_percent, not by slot order.
set -u

DAEMON="$(dirname "$0")/../claude-usage-daemon.sh"

# Pull just the two functions out of the daemon; sourcing it would start the loop.
eval "$(awk '/^read_codex_token\(\) \{/{f=1} f{print} f&&/^\}/{exit}' "$DAEMON")"
eval "$(awk '/^poll_codex\(\) \{/{f=1} f{print} f&&/^\}/{exit}' "$DAEMON")"
eval "$(awk '/^read_plan_label_for\(\) \{/{f=1} f{print} f&&/^\}/{exit}' "$DAEMON")"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

CODEX_AUTH_FILE="$TMP/auth.json"
CODEX_USAGE_URL="http://test.invalid/usage"
cat > "$CODEX_AUTH_FILE" <<'JSON'
{"auth_mode":"chatgpt","OPENAI_API_KEY":null,
 "tokens":{"id_token":"ID-WRONG","access_token":"ACCESS-RIGHT","refresh_token":"RT"},
 "last_refresh":"2026-07-10T16:04:49Z"}
JSON

FAKE_JSON=""
FAKE_RC=0
curl() { [ "$FAKE_RC" -ne 0 ] && return "$FAKE_RC"; printf '%s' "$FAKE_JSON"; return 0; }

fails=0
check() { # name want got
    if [ "$2" = "$3" ]; then
        echo "PASS: $1"
    else
        echo "FAIL: $1"
        echo "      expected [$2]"
        echo "      got      [$3]"
        fails=$((fails + 1))
    fi
}

# The fragment also carries the time-dependent cxra and optional cxpl keys, so
# the numeric core is asserted by prefix rather than exact match.
check_prefix() { # name want_prefix got
    case "$3" in
        "$2"*) echo "PASS: $1" ;;
        *) echo "FAIL: $1"; echo "      expected prefix [$2]"; echo "      got            [$3]"
           fails=$((fails + 1)) ;;
    esac
}

# --- token extraction: must take tokens.access_token, never id_token ---------
check "read_codex_token picks tokens.access_token" \
      "ACCESS-RIGHT" "$(read_codex_token)"

# --- real Plus shape: primary only, secondary null ---------------------------
FAKE_JSON='{"plan_type":"plus","rate_limit":{"primary_window":
{"used_percent":92,"limit_window_seconds":604800,"reset_after_seconds":595912},
"secondary_window":null}}'
check_prefix "primary-only window -> cx/cxr/cxw" \
      ',"cx":92,"cxr":9932,"cxw":10080' "$(poll_codex)"

# --- both windows present: most-USED wins, not slot order --------------------
FAKE_JSON='{"rate_limit":{"primary_window":
{"used_percent":30,"limit_window_seconds":18000,"reset_after_seconds":600},
"secondary_window":
{"used_percent":80,"limit_window_seconds":604800,"reset_after_seconds":86400}}}'
check_prefix "both windows -> picks the most-used one" \
      ',"cx":80,"cxr":1440,"cxw":10080' "$(poll_codex)"

# --- a 5h window must label itself as 5h (cxw drives the UI label) -----------
FAKE_JSON='{"rate_limit":{"primary_window":
{"used_percent":55,"limit_window_seconds":18000,"reset_after_seconds":3600},
"secondary_window":null}}'
check_prefix "5h window reports cxw=300" \
      ',"cx":55,"cxr":60,"cxw":300' "$(poll_codex)"

# --- degradation paths: all must yield an EMPTY fragment --------------------
FAKE_JSON='{"rate_limit":{"primary_window":null,"secondary_window":null}}'
check "both windows null -> empty" "" "$(poll_codex)"

FAKE_JSON='{"rate_limit":{}}'
check "no windows at all -> empty" "" "$(poll_codex)"

FAKE_JSON='not json at all'
check "malformed JSON -> empty" "" "$(poll_codex)"

FAKE_JSON=''
check "empty response -> empty" "" "$(poll_codex)"

FAKE_JSON='{"rate_limit":{"primary_window":{"used_percent":null}}}'
check "null used_percent -> empty" "" "$(poll_codex)"

FAKE_RC=7
check "curl failure -> empty" "" "$(poll_codex)"
FAKE_RC=0

# missing auth.json entirely (no Codex installed) — the common case for most users
rm -f "$CODEX_AUTH_FILE"
check "no auth.json -> empty" "" "$(poll_codex)"

# unreadable/garbage auth.json
echo 'garbage' > "$CODEX_AUTH_FILE"
check "garbage auth.json -> empty" "" "$(poll_codex)"

# --- Codex plan label + absolute reset ride along in the fragment -----------
cat > "$CODEX_AUTH_FILE" <<'JSON'
{"tokens":{"access_token":"ACCESS-RIGHT"}}
JSON
FAKE_JSON='{"plan_type":"plus","rate_limit":{"primary_window":
{"used_percent":42,"limit_window_seconds":604800,"reset_after_seconds":600,
 "reset_at":4102462800},"secondary_window":null}}'
got="$(poll_codex)"
case "$got" in
    *'"cxpl":"Codex Plus"'*) echo "PASS: codex plan label -> Codex Plus" ;;
    *) echo "FAIL: codex plan label missing/wrong in [$got]"; fails=$((fails + 1)) ;;
esac
case "$got" in
    *cxra*|*wra*) echo "FAIL: absolute reset keys should be gone, got [$got]"; fails=$((fails + 1)) ;;
    *) echo "PASS: no absolute-reset keys (countdown only)" ;;
esac

# plan_type absent must not emit an empty label
FAKE_JSON='{"rate_limit":{"primary_window":
{"used_percent":42,"limit_window_seconds":604800,"reset_after_seconds":600}}}'
case "$(poll_codex)" in
    *cxpl*) echo "FAIL: emitted cxpl with no plan_type"; fails=$((fails + 1)) ;;
    *) echo "PASS: no plan_type -> no cxpl key" ;;
esac

# --- Claude plan label from rateLimitTier -----------------------------------
plandir="$TMP/claude"; mkdir -p "$plandir"
cat > "$plandir/.credentials.json" <<'JSON'
{"claudeAiOauth":{"accessToken":"t","subscriptionType":"max","rateLimitTier":"default_claude_max_20x"}}
JSON
check "rateLimitTier -> 'Claude Max 20x' (not 20X)" \
      "Claude Max 20x" "$(read_plan_label_for "$plandir")"

cat > "$plandir/.credentials.json" <<'JSON'
{"claudeAiOauth":{"accessToken":"t","subscriptionType":"pro"}}
JSON
check "no rateLimitTier -> falls back to subscriptionType" \
      "Claude Pro" "$(read_plan_label_for "$plandir")"

cat > "$plandir/.credentials.json" <<'JSON'
{"claudeAiOauth":{"accessToken":"t"}}
JSON
check "no plan fields -> empty (subtitle stays blank)" \
      "" "$(read_plan_label_for "$plandir")"

check "missing credentials file -> empty" "" "$(read_plan_label_for "$TMP/nope")"

echo 'garbage' > "$plandir/.credentials.json"
check "garbage credentials -> empty" "" "$(read_plan_label_for "$plandir")"

echo
if [ "$fails" -eq 0 ]; then
    echo "All Codex daemon checks passed"
    exit 0
fi
echo "$fails check(s) failed"
exit 1
