#!/bin/bash
# Claude Code hook → Clawdmeter attention flag.
# Wire it in ~/.claude/settings.json:
#   Notification      → claude-attention-hook.sh notification
#   Stop              → claude-attention-hook.sh stop
#   UserPromptSubmit  → claude-attention-hook.sh prompt
# Writes an event type (input|perm|done|clear) into the flag file the BLE
# daemon watches. Reads the hook JSON from stdin.

set -u
DIR="$HOME/.config/claude-usage-monitor"
FLAG="$DIR/attention"
SESS="$DIR/sessions"   # per-session liveness: mtime = last heartbeat, content = fg|bg
MIN_TURN_S=45          # "done" only for turns longer than this — short Q&A means you're right there
IN=$(</dev/stdin)      # builtin read — no cat fork
[[ -d "$SESS" ]] || mkdir -p "$DIR" "$SESS"

# Hot path: PostToolUse fires on EVERY tool call of every session — handle it
# with zero subprocess spawns (bash regex instead of jq; session_id is the
# first field in the hook JSON, so the first match is the right one).
if [[ "${1:-}" == heartbeat ]]; then
    [[ "$IN" =~ \"session_id\"[[:space:]]*:[[:space:]]*\"([^\"]+)\" ]] \
        && echo fg > "$SESS/${BASH_REMATCH[1]}"
    exit 0
fi

jqr() { printf '%s' "$IN" | jq -r "$1 // \"\"" 2>/dev/null; }

# Does the session still have a live background task? A running task keeps its
# .output file open — lsof sees that. The tasks dir is keyed by the session's
# ORIGINAL project dir while the hook's cwd follows shell cd's, so locate it
# by globbing the unique session id. (NB: lsof's exit code is useless — 1 even
# with matches — test stdout.) Same heuristic as _bg_session_still_running in
# daemon/claude_usage_daemon.py — keep the two in sync.
has_running_tasks() {
    local t
    for t in /private/tmp/claude-$(id -u)/*/"$1"/tasks; do
        [[ -d "$t" ]] || continue
        lsof -w +d "$t" 2>/dev/null | grep -q . && return 0
        # Async agents don't hold their transcript open — the main process
        # appends it in bursts. Fresh writes count as running work too.
        [[ -n "$(find "$t" -type f -mtime -90s 2>/dev/null | head -1)" ]] && return 0
    done
    return 1
}

# Flag format: line 1 = event type, line 2 (optional) = project name shown on
# the device. Project = git repo root basename, falling back to cwd basename.
write_flag() {
    local type="$1" cwd; cwd=$(jqr '.cwd')
    local root=""
    [[ -n "$cwd" ]] && root=$(git -C "$cwd" rev-parse --show-toplevel 2>/dev/null)
    [[ -z "$root" ]] && root="$cwd"
    printf '%s\n%s\n' "$type" "$(basename "${root:-}")" > "$FLAG"
}

case "${1:-}" in
notification)
    # Claude is waiting: a permission prompt or an idle "waiting for your input".
    msg=$(jqr '.message' | tr '[:upper:]' '[:lower:]')
    if [[ "$msg" == *permission* || "$msg" == *разрешен* ]]; then
        write_flag perm
    else
        # Idle notifications also fire for autonomous sessions parked on their
        # own background work (builds, monitors, loops) — they're waiting for
        # the task, not for the user. Don't ring the bell for those.
        sid=$(jqr '.session_id')
        [[ -n "$sid" ]] && has_running_tasks "$sid" && exit 0
        write_flag input
    fi
    ;;
stop)
    sid=$(jqr '.session_id')
    # Don't celebrate a turn that merely yielded to a still-running background
    # task (build, flash, monitor) — the harness resumes it later.
    if [[ -n "$sid" ]]; then
        if has_running_tasks "$sid"; then
            echo bg > "$SESS/$sid"       # still working, just in the background
            exit 0
        fi
        rm -f "$SESS/$sid"               # turn really finished — session is idle
    fi
    # Skip short interactive turns — the user is at the keyboard anyway.
    if [[ -n "$sid" && -f "$DIR/turn-start-$sid" ]]; then
        started=$(cat "$DIR/turn-start-$sid" 2>/dev/null || echo 0)
        (( $(date +%s) - started < MIN_TURN_S )) && exit 0
    fi
    write_flag done
    ;;
prompt)
    # Any prompt — real or harness-generated — means the session is working.
    sid=$(jqr '.session_id')
    [[ -n "$sid" ]] && echo fg > "$SESS/$sid"
    # Harness-generated turns (background-task notifications, scheduled
    # wakeups) also fire UserPromptSubmit — they are NOT the user coming back,
    # so they must neither dismiss an alert nor restart the turn clock.
    p=$(jqr '.prompt')
    case "$p" in *"[SYSTEM NOTIFICATION"*|*"<task-notification>"*|*"<system-reminder>"*) exit 0;; esac
    # The user is typing: stamp the turn start and dismiss any pending alert.
    [[ -n "$sid" ]] && date +%s > "$DIR/turn-start-$sid"
    echo clear > "$FLAG"
    # One sweep covers both: turn-start stamps in $DIR and stale session
    # files in $SESS (which lives inside $DIR).
    find "$DIR" -type f \( -name 'turn-start-*' -o -path "$SESS/*" \) -mtime +1 -delete 2>/dev/null
    ;;
esac
exit 0
