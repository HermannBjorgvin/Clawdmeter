## Why

The host daemon is a single Python script hard-wired to Claude Code's Keychain/file-based OAuth token and the Anthropic API's rate-limit headers. It can't show usage for any other AI coding tool the user runs (Cursor, Kiro, Codex CLI, Gemini CLI/Antigravity), and distributing it requires a Python interpreter plus a per-OS venv install step (`install-mac.sh`, `install.sh`, `install-windows` flow) rather than a single downloadable binary. The user wants one daemon binary that can report usage for whichever supported tool(s) are installed, and that installs by copying one file.

## What Changes

- Rebuild the daemon in Rust, compiled to a standalone binary per OS/arch via `cargo build --release` — no Python, no Node/Bun, no interpreter of any kind required on the target machine.
- Introduce a provider-adapter architecture: each AI tool's credential lookup + usage polling is an isolated adapter behind a common interface, so the daemon can run zero or more providers concurrently and pick the "active" one using the same recency heuristic the current multi-config-dir logic uses.
- Ship working adapters for the two tools with a verified local usage API today:
  - **Claude Code** (parity with the current Python daemon: Keychain on macOS, `.credentials.json` on Linux/Windows, `api.anthropic.com` rate-limit headers).
  - **OpenAI Codex CLI** (`~/.codex/auth.json` token against the `chatgpt.com/backend-api/wham/usage` endpoint the Codex TUI itself polls).
- Explicitly do **not** ship Cursor, Kiro, Gemini CLI, or Antigravity adapters in this change, and document why in the README for each: Cursor's only usage API is org-scoped and needs a separate admin credential (not the individual's local session); Kiro's `/usage` data is only confirmed reachable through an interactive CLI session today, with no documented non-interactive/JSON path; neither Gemini CLI nor Antigravity expose any local usage/quota endpoint today. The provider interface must make it straightforward to add any of these later if that changes.
- **BREAKING**: the on-disk config format gains a `providers` section (which adapters to enable, in priority order) replacing the current `config_dirs`-only model; existing single-provider Claude configs must keep working with no edits (default `providers: [claude]` equivalent to today's behavior).
- Re-implement the existing BLE-central connection to the ESP32 "Clawdmeter" peripheral (same GATT service/characteristic UUIDs, same JSON wire payload) in Rust using the `btleplug` crate. This was initially attempted in Bun/TypeScript and hit two hard blockers there — noble's macOS binding can't reach a peripheral macOS already holds a connection to (an architectural CoreBluetooth gap), and `bun build --compile` crashes on noble's dependency tree — both confirmed by hands-on spikes, not assumption. A follow-up Rust spike, also run against the real board, **succeeded**: `btleplug` (patched with the still-unmerged `retrieveConnectedPeripheralsWithServices` support from [PR #437](https://github.com/deviceplug/btleplug/pull/437)) connected to the already-OS-paired board and wrote a payload successfully, compiled as a single ~1.5MB native binary with no bundling step at all. See design.md for the full comparison and the plan for depending on that not-yet-upstreamed capability.

## Capabilities

### New Capabilities
- `provider-adapter`: the pluggable interface for discovering local AI-tool credentials and polling usage/quota, the registry of which adapters exist (only `claude` and `codex` in this change) and which known providers are explicitly unsupported, and the active-provider selection logic when more than one is configured.
- `ble-transport`: connecting as a BLE central to the previously-paired "Clawdmeter" ESP32 peripheral via macOS's `retrieveConnectedPeripheralsWithServices` (and the equivalent BlueZ/WinRT approach on Linux/Windows), writing the usage JSON payload to its GATT characteristic, and handling the REQ/notify refresh-request flow — behaviorally identical wire contract to the current Python daemon, reimplemented in Rust and verified end-to-end against real hardware for the macOS leg.
- `binary-distribution`: producing single-file, dependency-free executables for macOS (arm64/x64), Linux (x64/arm64), and Windows (x64) via `cargo build --release`, plus the per-OS autostart integration (launchd/systemd/Windows Run key) that replaces today's shell/PowerShell installers.

### Modified Capabilities
(none — no existing capability specs are captured for the current Python daemon)

## Impact

- **Removed**: `daemon/claude-usage-daemon.sh`, `daemon/claude_usage_daemon.py`, `daemon/claude_usage_daemon_windows.py`, their `requirements*.txt`/venv setup, and the `install*.sh`/autostart Python scripts — superseded by the compiled binary and its own installer.
- **Added**: a Rust daemon project (new `daemon/` implementation; language/build tooling change from Python to Rust/Cargo), per-OS build artifacts, and a `providers` block in the daemon config file. Depends on a forked/patched `btleplug` branch until [PR #437](https://github.com/deviceplug/btleplug/pull/437) (or an equivalent) lands upstream — see design.md for the plan to not be permanently stuck on an unmerged fork.
- **Unchanged**: the ESP32 firmware side (`firmware/src/ble.cpp` and the GATT service/characteristic UUIDs, JSON payload schema) — this change is host-side only and must not require a firmware re-flash for existing boards.
- **Docs**: `README.md`'s macOS/Linux/Windows installation sections need a full rewrite around the new single-binary install flow.
