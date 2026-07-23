## 1. Spike: validate BLE-in-Rust across all three platforms

- [x] 1.1 Build the smallest possible Rust project using `btleplug` to reach a real "Clawdmeter" peripheral and write to its RX characteristic — done at `daemon-rs-spike/`. **Result on macOS: released btleplug is scan-only (same gap as noble's Bun binding) and cannot find Clawdmeter; patched with the `challiwill/btleplug@cjh/rebuild-features` branch (unmerged PR #437), `connected_peripherals()` found it immediately via `retrieveConnectedPeripheralsWithServices` with zero scanning.**
- [x] 1.2 Build `cargo build --release` and confirm the resulting single-file binary still connects and writes successfully (not just `cargo run`) — **confirmed**: a ~1.5MB standalone Mach-O binary, no bundler, connected and wrote on the first try.
- [ ] 1.3 Repeat 1.1/1.2 on Linux against real hardware — confirm whether released (unpatched) `btleplug`'s BlueZ backend can see an already-connected peripheral without the same patch macOS needed
- [ ] 1.4 Repeat 1.1/1.2 on Windows against real hardware if available; if not, document that Windows validation is still outstanding — pay attention to whether WinRT needs equivalent already-connected-peripheral handling (PR #437 touched a Windows feature flag, suggesting it might)
- [x] 1.5 Record the outcome as a decision update to design.md — done: Rust + patched `btleplug` is the chosen path; Bun/TypeScript was tried first and rejected (see design.md Context for the full comparison)
- [ ] 1.6 Decide and record how to depend on the unmerged PR long-term: track it for upstream merge, or vendor the ~50-line CoreBluetooth diff into our own module if it stalls (see design.md Decision 2 and Risks)

## 2. Project scaffolding

- [x] 2.1 Set up the new Rust daemon project — done at `daemon-rs/` (sibling to the legacy Python `daemon/`, per the "or a new sibling" option), `tokio` async runtime, `Cargo.toml` pinning the patched `btleplug` branch
- [x] 2.2 Define the `UsageProvider` trait, `NormalizedUsage` type, and the provider registry — `src/providers/mod.rs` (only `Stable` tier, as scoped)
- [x] 2.3 Port the config file loader — `src/config.rs`, flat `key = value` parser matching the real format (corrected from this task's original YAML/TOML assumption — see design.md Decision 3), comma-separated `providers` key defaulting to `claude`
- [x] 2.4 Implement the poll loop — `src/main.rs`, including the ported `PlanSelector` active-provider selection logic (see provider-adapter spec)
- [x] 2.5 Implement the "unsupported provider identifier" rejection path — `build_registry()` in `src/providers/mod.rs`

## 3. Claude Code provider adapter (stable)

- [x] 3.1 Port macOS Keychain token read and file-vs-Keychain precedence — `src/providers/claude.rs` (shelled out to `security`, not the `security-framework` crate, for simplicity)
- [x] 3.2 Port the credentials-file extraction logic, including the mcpOAuth-vs-claudeAiOauth disambiguation
- [x] 3.3 Port rate-limit polling + normalization for both Pro/Max and Enterprise branches, including the calendar-monthly billing-period math (`chrono`)
- [x] 3.4 Port multi-`config_dirs` support and per-dir independent polling
- [x] 3.5 Write tests reproducing the stale-credentials-file bug class — 4 unit tests in `src/providers/claude.rs`, all passing. Went further than "log it": a file token's `expiresAt` is now checked proactively and an expired file token is skipped in favor of Keychain, rather than attempted and only failing via a 401.

## 4. OpenAI Codex CLI provider adapter (stable)

- [x] 4.1 Implement `~/.codex/auth.json` token discovery — `src/providers/codex.rs`
- [x] 4.2 Implement the `chatgpt.com/backend-api/wham/usage` poll — implemented, but **UNVERIFIED**: I have no Codex CLI account to test against, so the response-shape parsing (`five_hour`/`weekly` fields) is a best-effort guess documented as such in the code, with defensive fallback (logs the raw response and returns "no data" rather than fabricating numbers if the shape doesn't match). Needs a real test pass before this can be called done.
- [x] 4.3 Normalize Codex's fields onto the shared shape — implemented per the guessed shape above; same verification caveat applies
- [x] 4.4 Handle missing/invalid credentials and endpoint errors gracefully — implemented (no panics, degrades to "no data this cycle")

## 5. BLE transport

- [x] 5.1 Wrap the spike's flow into `src/ble.rs`
- [x] 5.2 RX-characteristic JSON write — implemented and matches firmware field names
- [x] 5.3 REQ-characteristic handling — implemented as a proper subscribe+wait-for-notification (REQ is NOTIFY-only, no READ property — an initial read-based draft was wrong; caught by checking `firmware/src/ble.cpp`, not by testing)
- [x] 5.4 Reconnection handling — each poll cycle independently re-resolves the peripheral via `find_peripheral`, so a dropped/reconnected OS-level connection is naturally picked up next cycle with no persistent-connection state to get stuck
- [ ] 5.5 Apply Linux/Windows-specific handling — blocked on 1.3/1.4, which are blocked on hardware access
- [ ] 5.6 Manually verify against a real flashed board — **in progress, blocked mid-verification**: the compiled daemon ran against the real board but failed with "Clawdmeter not currently connected"; root-caused to the board's OS-level Bluetooth connection having dropped (system_profiler no longer shows a live battery-level reading for it, only pairing), not a code bug — this exact "not connected" outcome is the spec's own defined correct behavior for that state. **Needs the board reconnected via System Settings → Bluetooth to complete this verification.** Two real bugs were found and fixed during this attempt: (a) checking for a device refresh-request on every 5s tick raced against the main poll's own BLE connection — fixed by only checking when not already polling; (b) the CoreBluetooth cache-population wait was 500ms, under the 2s the spike had already verified was needed.

## 6. Distribution & packaging

- [ ] 6.1 Set up GitHub Actions matrix builds (native runners per OS) producing `cargo build --release` artifacts for macOS arm64/x64, Linux x64/arm64, Windows x64
- [ ] 6.2 Publish these binaries via GitHub Releases (or a documented manual process if CI isn't set up yet)
- [ ] 6.3 Rewrite `install-mac.sh` to install the compiled binary and register a LaunchAgent pointing at it directly; document the one-time Bluetooth-permission grant the bare binary needs (same requirement the current Python daemon already has)
- [ ] 6.4 Rewrite `install.sh` (Linux) to install the binary and register a systemd user unit
- [ ] 6.5 Rewrite the Windows install flow to install the binary and register a Run-key autostart entry (decide per design.md Open Questions whether the tray UI is preserved or dropped for this release)
- [ ] 6.6 Each installer must detect and disable any existing legacy Python daemon autostart entry before registering the new one, to prevent double-polling

## 7. Migration, rollback, and legacy retention

- [ ] 7.1 Move (not delete) `daemon/claude-usage-daemon.sh`, `claude_usage_daemon.py`, `claude_usage_daemon_windows.py`, and their requirements/venv setup into a clearly marked legacy location
- [ ] 7.2 Add a `--legacy` (or equivalently named) rollback path to the installers that re-registers the legacy Python daemon and disables the new binary's autostart entry
- [ ] 7.3 Confirm a config file with no `providers` key produces identical behavior on both the legacy and new daemon, side by side, before removing the legacy code path in a future release
- [ ] 7.4 Clean up (or formally fold in) the `daemon-rs-spike/` and `daemon-ts-spike/` exploratory projects once the real `daemon/` implementation supersedes them

## 8. Documentation

- [ ] 8.1 Rewrite `README.md`'s macOS/Linux/Windows installation sections around the single-binary install flow
- [ ] 8.2 Document the `providers` config option and that only `claude`/`codex` are implemented
- [ ] 8.3 Add a README section (e.g. "Other providers") giving each unsupported tool its own entry with the specific technical reason it isn't included yet:
      - **Cursor**: only usage API is org-scoped (separate admin API key), not the individual's local session token
      - **Kiro**: has an official `/usage` command, but it's confirmed reachable only in an interactive CLI session, not a scripted/JSON path — revisit if that changes
      - **Gemini CLI**: no local usage/quota endpoint exists today (open question in Google's own developer forum)
      - **Google Antigravity**: no local usage/quota endpoint exists today, and its quota model is an opaque "agent work" metric rather than a simple percentage
- [ ] 8.4 Document the one-time macOS Bluetooth-permission grant the bare binary needs on first run (same as today's Python daemon requires)
- [ ] 8.5 Document the rollback path to the legacy Python daemon
- [ ] 8.6 Update `daemon/config.example` with the new `providers` field and inline comments referencing the "Other providers" README section for anything not listed
