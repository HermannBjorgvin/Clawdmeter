## ADDED Requirements

### Requirement: Standalone single-file executables per platform
The system SHALL be distributed as a standalone executable produced by `cargo build --release` for macOS (arm64, x64), Linux (x64, arm64), and Windows (x64), requiring no separately installed runtime (no system Python, Node, or Bun) on the target machine on any of the three platforms.

#### Scenario: Runs on a machine with no Python/Node/Bun installed
- **WHEN** a user downloads the appropriate platform binary and runs it on a machine with no scripting-language runtime installed
- **THEN** the daemon starts and operates normally, including its BLE transport

### Requirement: Existing single-provider configuration keeps working unchanged
The system SHALL treat a configuration file with no `providers` key as equivalent to `providers: [claude]`, so that a user upgrading from the current Python daemon's config format does not need to edit their config file for the daemon to keep functioning as before.

#### Scenario: Old config file works with the new binary
- **WHEN** a config file written for the current Python daemon (no `providers` key, only `config_dirs`) is used with the new binary
- **THEN** the daemon behaves exactly as it did before the rewrite: Claude-only, same config-dir handling

### Requirement: Per-OS autostart registration replaces the shell/venv installers
The system SHALL provide an installation path that registers the compiled binary directly with each OS's native autostart mechanism (a macOS LaunchAgent, a Linux systemd user unit, or a Windows Run-key entry) pointing at the binary's final location, without creating or depending on a Python virtual environment on any platform.

#### Scenario: Fresh install requires no venv or interpreter setup
- **WHEN** a user runs the new installer on a clean machine, on any of the three supported platforms
- **THEN** the daemon is registered to start automatically at login without any Python venv or other interpreter setup being created

### Requirement: Legacy Python daemon remains available for one release as a rollback path
The system SHALL keep the existing Python daemon implementation and its installers in the repository, functioning and documented, for at least one release after the compiled binary ships, so a user can roll back if the new binary regresses for their platform.

#### Scenario: User rolls back to the Python daemon
- **WHEN** a user runs the legacy install script after having installed the new binary
- **THEN** the legacy installer disables the new binary's autostart entry and registers the Python daemon's autostart entry instead, leaving the system in a working single-daemon state
