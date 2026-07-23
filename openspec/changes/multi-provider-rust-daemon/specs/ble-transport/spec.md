## ADDED Requirements

### Requirement: Connect only to an already system-paired peripheral
The system SHALL connect as a BLE central only to a peripheral named "Clawdmeter" that the host OS already has paired/connected at the Bluetooth-stack level, by querying for already-connected peripherals matching the daemon's GATT service UUID rather than performing its own advertisement scan. The daemon MUST NOT perform its own BLE scan-and-pair flow; pairing remains a one-time manual step the user performs in the OS's Bluetooth settings, matching current behavior. On macOS this specifically means calling the equivalent of `CBCentralManager.retrieveConnectedPeripheralsWithServices` — a plain advertisement scan is verified NOT to find the peripheral, since macOS excludes already-connected peripherals from scan results.

#### Scenario: No system-level connection means no daemon action
- **WHEN** the OS has not connected to any "Clawdmeter" peripheral
- **THEN** the daemon does not attempt to write usage data and logs that no peripheral is available, retrying on the next poll cycle

#### Scenario: Daemon picks up an OS-level connection on its next cycle
- **WHEN** the user connects to "Clawdmeter" via the OS Bluetooth settings while the daemon is running
- **THEN** the daemon detects the connection within one poll interval and begins writing usage data

#### Scenario: Retrieval works even though the peripheral never appears in a scan
- **WHEN** the peripheral is already connected to the OS (and, per current hardware, is also a bonded BLE HID keyboard that will not appear in a normal advertisement scan)
- **THEN** the daemon still successfully finds and connects to it via the already-connected-peripheral query, not scanning

### Requirement: Usage payload write contract is unchanged from the current firmware
The system SHALL write usage data as a single JSON line to the GATT characteristic UUID `4c41555a-4465-7669-6365-000000000002` on service `4c41555a-4465-7669-6365-000000000001`, using the same field names the firmware already parses (`s`, `sr`, `w`, `wr`, `st`, `c`, `acct`, `tp`, `pd`, `rd`, `t`, `tf`, `ok`). No firmware change is required for this daemon rewrite to function.

#### Scenario: Existing firmware accepts the rewritten daemon's payload unmodified
- **WHEN** the new daemon writes a usage payload to an ESP32 running firmware built before this change
- **THEN** the firmware parses it successfully and updates the display, with no firmware re-flash required

### Requirement: Device-initiated refresh requests trigger an immediate poll
The system SHALL monitor GATT characteristic UUID `...0004` for a device-initiated refresh notification and, upon receiving one, poll all enabled providers immediately rather than waiting for the next scheduled interval.

#### Scenario: Fresh device requests data on first connection
- **WHEN** the ESP32 notifies on the REQ characteristic because it has not yet received data since boot
- **THEN** the daemon polls providers immediately and writes the result, without waiting for the normal poll interval

### Requirement: Reconnection resilience
The system SHALL detect a lost BLE connection and resume writing usage data automatically once the OS re-establishes the connection, without requiring the daemon to be restarted.

#### Scenario: Daemon recovers after the peripheral goes out of range
- **WHEN** the BLE connection drops and the OS later reconnects to "Clawdmeter"
- **THEN** the daemon resumes polling and writing usage data without manual intervention

### Requirement: BLE connectivity is available on macOS, Linux, and Windows in-process
The system SHALL provide the above BLE-central behaviors on macOS, Linux, and Windows within the main compiled executable's own process, with no separately-installed runtime or spawned helper process required. This is verified on macOS (via `btleplug`, patched for the already-connected-peripheral gap); Linux and Windows are expected to work via `btleplug`'s existing BlueZ/WinRT backends but must be verified against real hardware before being considered complete, since each platform's BLE stack has its own quirks around already-connected peripherals.

#### Scenario: End-to-end behavior is identical regardless of OS-specific BLE stack
- **WHEN** a user on any of the three supported OSes installs and runs the daemon
- **THEN** usage data reaches the display without the user needing to install, configure, or run any additional BLE-related software themselves

#### Scenario: A platform-specific gap surfaces as a documented exception, not a silent failure
- **WHEN** a target platform's BLE stack turns out to need different handling than macOS (e.g. an equivalent already-connected-peripheral gap)
- **THEN** that platform's specific handling is implemented and verified against real hardware before being marked supported, rather than assumed to work because macOS did
