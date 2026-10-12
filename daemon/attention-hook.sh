#!/bin/sh
# Claude Code hook for the attention card (see README, "Attention alerts").
#   attention-hook.sh alert   Notification, PreToolUse(AskUserQuestion)
#   attention-hook.sh clear   UserPromptSubmit, PostToolUse
# Hook stdout can reach the model, so print nothing and always exit 0.
exec >/dev/null 2>&1
PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
D="$HOME/.config/claude-usage-monitor"
mkdir -p "$D" || exit 0
case "$1" in
  alert)
    jq -c '{m: (.message // .tool_input.questions[0].question // ""),
            s: .session_id}' > "$D/attention.$$" \
      && mv "$D/attention.$$" "$D/attention" ;;
  clear)
    s=$(jq -r '.session_id // empty') && [ -n "$s" ] && touch "$D/clear-$s" ;;
esac
exit 0
