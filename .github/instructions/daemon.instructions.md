---
applyTo: "daemon/**,install*.sh,flash*.sh,install-mac.sh"
description: "Use for host daemon, BLE transport, credentials, installers, and platform service integration."
---

# Daemon Rules

- Preserve firmware payload compatibility and the BLE UUID contract.
- Keep credentials out of logs, fixtures, and documentation.
- Consider Windows Scheduled Task, macOS LaunchAgent, and Linux systemd behavior separately.
- Validate reconnect, refresh-request, stale-status, and missing-token paths.
- Prefer focused Python syntax/import or fixture checks before integration testing.
