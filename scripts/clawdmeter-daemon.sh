#!/bin/bash
# Manually start the Clawdmeter usage daemon (no launchd — by design).
# Reads the Claude Code OAuth token from the macOS Keychain, polls usage
# every ~60s, pushes to the paired Clawdmeter peripheral over BLE.
# Usage: scripts/clawdmeter-daemon.sh
set -euo pipefail
cd "$(dirname "$0")/../daemon"
exec ./.venv/bin/python claude_usage_daemon.py
