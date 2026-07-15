#!/bin/bash
# Clawdmeter usage daemon — manual runner (no launchd, by design).
#
# Reads the Claude Code OAuth token from the macOS Keychain, polls usage
# every ~60s, pushes it to the paired "Clawdmeter" peripheral over BLE.
#
# Usage:
#   scripts/clawdmeter-daemon.sh            # start in background (default)
#   scripts/clawdmeter-daemon.sh run        # run in foreground (Ctrl-C to stop)
#   scripts/clawdmeter-daemon.sh stop       # stop the background daemon
#   scripts/clawdmeter-daemon.sh status     # is it running?
#   scripts/clawdmeter-daemon.sh log        # follow the log
#
# First run auto-creates daemon/.venv with bleak + httpx.
set -euo pipefail

DAEMON_DIR="$(cd "$(dirname "$0")/../daemon" && pwd)"
VENV="$DAEMON_DIR/.venv"
PIDFILE="${TMPDIR:-/tmp}/clawdmeter-daemon.pid"
LOGFILE="$HOME/Library/Logs/clawdmeter-daemon.log"

ensure_venv() {
    if [ ! -x "$VENV/bin/python" ]; then
        echo "Creating venv + installing bleak/httpx (first run)..."
        python3 -m venv "$VENV"
        "$VENV/bin/pip" install -q bleak httpx
    fi
}

is_running() {
    [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null
}

case "${1:-start}" in
start)
    if is_running; then
        echo "Already running (pid $(cat "$PIDFILE")). Log: $LOGFILE"
        exit 0
    fi
    ensure_venv
    mkdir -p "$(dirname "$LOGFILE")"
    cd "$DAEMON_DIR"
    nohup "$VENV/bin/python" -u claude_usage_daemon.py >> "$LOGFILE" 2>&1 &
    echo $! > "$PIDFILE"
    sleep 2
    if is_running; then
        echo "Started (pid $(cat "$PIDFILE")). Log: $LOGFILE"
        tail -3 "$LOGFILE" 2>/dev/null || true
    else
        echo "Failed to start — last log lines:" >&2
        tail -10 "$LOGFILE" >&2 || true
        rm -f "$PIDFILE"
        exit 1
    fi
    ;;
run)
    ensure_venv
    cd "$DAEMON_DIR"
    exec "$VENV/bin/python" -u claude_usage_daemon.py
    ;;
stop)
    if is_running; then
        kill "$(cat "$PIDFILE")" && rm -f "$PIDFILE"
        echo "Stopped."
    else
        rm -f "$PIDFILE"
        # Catch strays started outside this script too.
        pkill -f claude_usage_daemon.py 2>/dev/null && echo "Stopped (stray)." || echo "Not running."
    fi
    ;;
status)
    if is_running; then
        echo "Running (pid $(cat "$PIDFILE")). Log: $LOGFILE"
        tail -3 "$LOGFILE" 2>/dev/null || true
    elif pgrep -f claude_usage_daemon.py > /dev/null 2>&1; then
        echo "Running outside this script (pid $(pgrep -f claude_usage_daemon.py | head -1))."
    else
        echo "Not running."
    fi
    ;;
log)
    exec tail -F "$LOGFILE"
    ;;
*)
    echo "Usage: $0 [start|run|stop|status|log]" >&2
    exit 1
    ;;
esac
