#!/bin/sh
# Claude Code hook for Clawdmeter (see README, "Attention alerts").
#
#   attention-hook.sh alert         Notification, PreToolUse(AskUserQuestion)
#   attention-hook.sh clear         UserPromptSubmit, PostToolUse
#   attention-hook.sh event         every Clawdmeter hook (the "fast clock")
#   attention-hook.sh alert event   alert + event from one hook entry
#   attention-hook.sh clear event   clear + event from one hook entry
#
# alert / clear drive the attention card: alert writes
# ~/.config/claude-usage-monitor/attention, clear touches clear-<session_id>.
#
# event appends one line to ~/.config/claude-usage-monitor/events.log:
#     <unix_ts> <hook_event_name> <session_id>
# Nothing from the prompt or the tool call is recorded. The daemon tails this
# file for the payload's a / e / es / n fields (activity, latest event,
# sessions active). The log rolls to events.log.1 past ~256 KB.
#
# Wiring in ~/.claude/settings.json (one entry per event; the combined modes
# read stdin once, so each hook stays a single process):
#   "UserPromptSubmit": [{ "hooks": [{ "type": "command",
#       "command": "/path/to/attention-hook.sh clear event" }] }],
#   "PostToolUse":      [{ "hooks": [{ "type": "command",
#       "command": "/path/to/attention-hook.sh clear event" }] }],
#   "Stop":             [{ "hooks": [{ "type": "command",
#       "command": "/path/to/attention-hook.sh event" }] }],
#   "Notification":     [{ "hooks": [{ "type": "command",
#       "command": "/path/to/attention-hook.sh alert event" }] }],
#   "PreToolUse":       [{ "matcher": "AskUserQuestion", "hooks": [{ "type": "command",
#       "command": "/path/to/attention-hook.sh alert event" }] }]
# Existing installs that pass only alert / clear keep working unchanged.
#
# Hook stdout can reach the model, so print nothing and always exit 0. Never
# blocks: no network, no locks, one short append.
exec >/dev/null 2>&1
PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
D="$HOME/.config/claude-usage-monitor"
mkdir -p "$D" || exit 0

log_event() {  # stdin: hook JSON
  line=$(jq -r '[(.hook_event_name // "unknown"), (.session_id // "-")]
                | map(tostring | gsub("[^A-Za-z0-9_.-]"; "")) | join(" ")') || return
  [ -n "$line" ] || return
  f="$D/events.log"
  if [ -f "$f" ] && [ "$(wc -c < "$f")" -gt 262144 ]; then mv -f "$f" "$f.1"; fi
  printf '%s %s\n' "$(date +%s)" "$line" >> "$f"
}

if [ "$2" = event ]; then
  # Combined mode: read the hook JSON once, feed both handlers.
  in=$(cat)
  printf '%s' "$in" | log_event
  exec <<EOF
$in
EOF
fi

case "$1" in
  alert)
    jq -c '{m: (.message // .tool_input.questions[0].question // ""),
            s: .session_id}' > "$D/attention.$$" \
      && mv "$D/attention.$$" "$D/attention" ;;
  clear)
    s=$(jq -r '.session_id // empty') && [ -n "$s" ] && touch "$D/clear-$s" ;;
  event)
    log_event ;;
esac
exit 0
