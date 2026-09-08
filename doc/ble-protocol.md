# BLE Protocol Contract

The firmware advertises as `Claude Controller`.

## Service and Characteristics

| Purpose | UUID | Direction |
| --- | --- | --- |
| Data service | `4c41555a-4465-7669-6365-000000000001` | Device service |
| RX | `4c41555a-4465-7669-6365-000000000002` | Host writes JSON |
| TX | `4c41555a-4465-7669-6365-000000000003` | Firmware notifications |
| REQ | `4c41555a-4465-7669-6365-000000000004` | Firmware refresh request |

## Payload Routing

The `src` field selects the firmware update path. Current values include `claude` (default), `status`, `copilot`, `sysinfo`, `vscode`, `act`, `ci`, `sum`, and `env`. Field names and defaults are implemented in `firmware/src/main.cpp` and produced by `daemon/claude_usage_daemon.py`.

## Compatibility Rules

- Do not change UUIDs, device name, or existing payload keys without an explicit protocol migration.
- Update both daemon and firmware parsing when adding a payload.
- Preserve missing-data defaults so older daemons do not produce undefined UI state.
- Validate changes with representative JSON through the firmware `feed` serial command when hardware is available.
- Document new `src` values and keys here.
