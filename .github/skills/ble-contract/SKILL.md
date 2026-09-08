---
name: ble-contract
description: "Use when changing or reviewing BLE GATT behavior, daemon payloads, firmware JSON parsing, reconnect logic, refresh requests, or protocol compatibility."
---

# BLE Contract

Review both sides together: `firmware/src/ble.cpp`, `firmware/src/data.h`, `firmware/src/main.cpp`, `daemon/claude_usage_daemon.py`, and the shell daemon.

Preserve unless explicitly migrated:
- service `4c41555a-4465-7669-6365-000000000001`
- RX `...0002`, TX `...0003`, REQ `...0004`
- device name `Claude Controller`
- existing `src` values and payload keys
- request/subscribe and reconnect behavior

For changes, add or update a payload fixture, verify parser routing, and document compatibility and failure behavior.
