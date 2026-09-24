#!/bin/bash
# Regression test for read_token_for() in claude-usage-daemon.sh.
#
# A real ~/.claude/.credentials.json holds many "accessToken" fields — one per
# OAuth integration (MCP servers, design tools) plus the Claude subscription
# token under claudeAiOauth. read_token_for() must return ONLY the claudeAiOauth
# token; concatenating all of them produces an invalid Bearer header and every
# API call 401s (symptom: device shows "unknown"/0% forever).
set -u

DAEMON="$(dirname "$0")/../claude-usage-daemon.sh"

# Pull just the read_token_for function body out of the daemon (sourcing the
# whole file would start the daemon loop) and eval it here.
eval "$(awk '/^read_token_for\(\) \{/{f=1} f{print} f&&/^\}/{exit}' "$DAEMON")"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cat > "$TMP/.credentials.json" <<'JSON'
{"mcpOAuth":{"contentful|abc":{"accessToken":"mcp-contentful-TOKEN","expiresAt":0}},
"claudeAiOauth":{"accessToken":"sk-ant-oat01-CLAUDE-REAL","refreshToken":"rt","expiresAt":1783620177377,"scopes":["a","b"],"subscriptionType":"max"},
"designOauth":{"accessToken":"sk-ant-oat01-DESIGN-WRONG","refreshToken":"rt2"}}
JSON

got="$(read_token_for "$TMP")"
want="sk-ant-oat01-CLAUDE-REAL"

if [ "$got" = "$want" ]; then
    echo "PASS: read_token_for returned the claudeAiOauth token"
    exit 0
else
    echo "FAIL: expected [$want]"
    echo "      got      [$got]"
    exit 1
fi
