## Context

The current daemon is three parallel, mostly-duplicated implementations (`daemon/claude-usage-daemon.sh` for Linux/BlueZ, `daemon/claude_usage_daemon.py` for macOS/bleak, `daemon/claude_usage_daemon_windows.py` for Windows/bleak), each hard-coded to one provider (Claude Code) and installed via a per-OS script that sets up a Python venv. The wire contract to the ESP32 is simple and stable: connect as a BLE central to the peripheral named "Clawdmeter", write a JSON line to GATT characteristic `...0002`, watch `...0004` for a device-initiated refresh request, and don't bother subscribing to the `...0003` ack/nack notify (the firmware fires it but nothing currently reads it). That contract is out of scope for this change — only the host side is being rebuilt.

**This design went through two implementation spikes before settling on Rust**, both run against the real board rather than assumed from research:

1. **Bun/TypeScript spike (rejected).** `@stoprocent/noble`'s scan genuinely worked (it found a dozen real nearby BLE devices) but could never see Clawdmeter, because macOS's CoreBluetooth deliberately excludes peripherals it's already connected to from scan results — the board is also a bonded BLE HID keyboard, so it's always already OS-connected. Reaching it requires `CBCentralManager.retrieveConnectedPeripheralsWithServices`, which noble's macOS binding doesn't implement at all (confirmed by reading its source). Separately, `bun build --compile` crashed at runtime on noble's bundled dependency tree — reproduced on two Bun versions (v1.2.0 and v1.3.14) — a bundler-level bug unrelated to the CoreBluetooth issue, and likely not macOS-specific since the broken code was platform-conditional code bundled for all three OSes at once.
2. **Rust spike (succeeded).** Released `btleplug` has the identical scan-only limitation as noble — this is an Apple API gap, not a JavaScript-specific one. But `btleplug` has an open, unmerged PR ([#437](https://github.com/deviceplug/btleplug/pull/437)) that adds a `connected_peripherals()` method calling `retrieveConnectedPeripheralsWithServices` via `objc2-core-bluetooth`, an actively-maintained Rust binding to Apple's own framework. Building against that PR's branch, a minimal Rust program found Clawdmeter via `connected_peripherals()` (zero scanning), connected, and wrote to its RX characteristic successfully — both in `cargo build` and `cargo build --release` (a single ~1.5MB native Mach-O binary, no bundling step of any kind, since Rust binaries are the compiled artifact, not a bundled scripting runtime).

Provider usage APIs vary from "documented" to "doesn't exist," independent of the language choice: only Claude Code (existing) and OpenAI Codex CLI (undocumented but concretely reproducible — the Codex TUI's own rate-limit poller hits `chatgpt.com/backend-api/wham/usage` with the token from `~/.codex/auth.json`) have a usage-checking path solid enough to ship as "stable" — this change scopes to exactly those two. Kiro, Cursor, Gemini CLI, and Antigravity are all researched and deliberately excluded (see proposal.md); each needs its own follow-up spike before a future change can add it.

## Goals / Non-Goals

**Goals:**
- One daemon binary per OS/arch, genuinely no runtime (no Python, no Node, no Bun) required on the target machine — confirmed achievable on macOS via the Rust spike; Linux and Windows still need their own confirmation (see Open Questions).
- Pluggable provider architecture so adding a 5th, 6th provider later is "write an adapter," not "fork the daemon."
- Preserve every behavior a current user depends on: multi-config-dir active-plan selection, macOS Keychain token reads, the exact BLE wire payload the firmware expects, and autostart-on-login.
- Ship Claude + Codex CLI as fully supported on day one; leave Cursor, Kiro, Gemini CLI, and Antigravity as documented non-goals with a clear extension point and README notes explaining why each isn't supported yet.

**Non-Goals:**
- Changing anything on the ESP32/firmware side. This is a host-only rewrite.
- Supporting Cursor, Kiro, Gemini CLI, or Antigravity in this change. Cursor's only usage API is org-scoped; Kiro's `/usage` data is confirmed reachable only through an interactive CLI session, not a documented non-interactive/JSON path; Gemini CLI and Antigravity expose no local usage/quota endpoint today (see Context). The adapter interface must not preclude adding any of these later — this is scope discipline for this change, not a permanent architectural exclusion.
- A GUI/tray app rewrite. The existing Windows tray app (`pystray`-based) is out of scope; this change can leave Windows on a console/background-process model for v1.
- Getting btleplug PR #437 merged upstream ourselves as a hard requirement — depending on a patched fork for one release is acceptable (see Decision 2 and its risk mitigation) while that's sorted out, either by us or upstream.

## Decisions

### 1. Provider adapters are a small, uniform trait, not per-provider special-casing in the poll loop
Each provider implements:
```rust
#[async_trait]
trait UsageProvider {
    fn id(&self) -> &'static str;             // "claude" | "codex" in this change
    fn support_tier(&self) -> SupportTier;     // only Stable is used in this change
    async fn find_credential(&self) -> Option<Credential>;   // OS keychain / file lookup
    async fn poll_usage(&self, cred: &Credential) -> Option<NormalizedUsage>; // one HTTP call
}
```
`NormalizedUsage` maps 1:1 onto the existing wire payload fields (`s`, `sr`, `w`, `wr`, `st`, `acct`, `tp`, `pd`, `rd`, `ok`) — see `firmware/src/data.h` for field semantics. This keeps the ESP32 firmware, and the active-plan-selection logic, provider-agnostic: the daemon polls every enabled provider each cycle and picks the one with the most recent upward movement in session usage, exactly like today's multi-config-dir logic, just generalized from "multiple Claude config dirs" to "multiple providers and/or config dirs."
- **Alternative considered**: one big `poll()` function with an `if provider == "claude"` branch per tool, matching the current script's structure. Rejected — the whole point of this change is that adding a provider shouldn't touch shared code, and the current Python daemon's file-vs-Keychain-vs-multi-dir logic in `read_token_for()` is exactly the kind of tangled special-casing (see the stale-credentials-file bug class we already hit once) this interface is meant to prevent.

### 2. BLE transport: `btleplug`, patched for macOS's missing `connected_peripherals`, until it's upstreamed
Use `btleplug` for all three platforms' BLE-central needs. On macOS specifically, depend on a git branch (`challiwill/btleplug@cjh/rebuild-features`, the branch behind [PR #437](https://github.com/deviceplug/btleplug/pull/437)) rather than the released crate, since the released version can't reach an already-OS-connected peripheral at all — confirmed both by reading its CoreBluetooth backend source (no `retrieveConnectedPeripheralsWithServices` call anywhere) and by the spike's real-hardware test succeeding only against the patched branch.
- To avoid being permanently stuck on someone else's unmerged, possibly-abandoned fork: vendor the specific ~50-line CoreBluetooth diff (already small and self-contained — see the PR) into our own thin wrapper module if the upstream PR stalls past this change's timeline, rather than depending on the git branch indefinitely. Track this as a follow-up task (see tasks.md) rather than blocking on it now.
- Linux (BlueZ via `bluer`/D-Bus) is expected to work with the released `btleplug` unmodified — per research, BlueZ shows already-connected devices in scans by default, unlike CoreBluetooth. This needs its own hands-on spike (task 1.3) to confirm rather than assume.
- Windows (WinRT) touched the same capability gap in the PR's diff (it added a `Devices_Enumeration` feature flag), suggesting Windows may need similar already-connected-peripheral handling. Needs its own spike (task 1.4) — don't assume Windows "just works" because macOS did.
- **Alternative considered**: a native Swift/Obj-C helper binary calling CoreBluetooth directly, bypassing btleplug/objc2 entirely. Rejected as unnecessary extra work — `objc2-core-bluetooth` (the binding btleplug's fork already uses) is itself an actively-maintained, general-purpose Rust↔Apple-framework binding; there's no need to hand-roll the same capability again in a different language.
- **Alternative considered**: keep the existing Python+bleak scripts as a spawned helper subprocess (this change's plan before the Rust spike). Superseded — the Rust spike proved a fully in-process, dependency-free solution exists on macOS, which is strictly better than a helper-process architecture if it holds up on Linux/Windows too.

### 3. Config format gains a `providers` key; old single-Claude config is the implicit default
The existing config file is a flat `key = value` format (see `daemon/config.example` — not YAML), read fresh on every poll cycle. `providers` follows the same comma-separated-list convention `config_dirs` already uses:
```
# providers = claude, codex   # default when unset: claude only (today's behavior)
# cursor, kiro, gemini, antigravity are not yet implemented — see README
config_dirs = ~/.claude, ~/.claude-work   # unchanged; still Claude-specific (per-tool config dirs vary)
```
A config file with no `providers` key behaves exactly like today (Claude only), so existing installs don't need to change anything to keep working after a binary swap.
- **Alternative considered**: auto-detect every installed tool and enable all adapters with usable credentials by default. Rejected for v1 — with only two stable adapters this adds little value now, and it would need revisiting anyway once an experimental tier exists for a future provider.

### 4. Distribution: `cargo build --release` per target triple, GitHub Releases artifacts, thin OS-native installers replace the shell scripts
`cargo build --release --target=<triple>` produces the binaries (via GitHub Actions matrix runners — one native runner per OS is simpler and more reliable than cross-compiling Linux/Windows targets from a single macOS machine, especially while the BLE crate stack has platform-specific native dependencies); `install-mac.sh`/`install.sh`/Windows install flow are rewritten as much smaller scripts that just: download or locate the right binary, drop it in a stable location, and register the OS's native autostart mechanism (launchd plist / systemd user unit / Windows Run key) pointing at that binary directly.

## Risks / Trade-offs

- **[Risk] Depending on an unmerged, possibly-abandoned btleplug fork** → Mitigation: the diff is small (~50 lines, self-contained to the CoreBluetooth backend); vendor it into our own module if the upstream PR stalls (see Decision 2). Track PR #437's status and re-evaluate before each release.
- **[Risk] macOS Bluetooth permission (TCC) friction for a bare, unbundled binary** → Confirmed real but language-agnostic (also affects Node/noble, also documented against btleplug itself in [issue #106](https://github.com/deviceplug/btleplug/issues/106)): a bare CLI binary has no `Info.plist`, so first-run Bluetooth access needs the OS to prompt for (and the user to grant) permission to the process/terminal launching it. Mitigation: document this clearly in the macOS install instructions (the current Python daemon already has this exact requirement today, so it's not a regression); consider wrapping the binary in a minimal signed `.app` bundle in a later release if the bare-binary UX proves too rough.
- **[Risk] Codex CLI's usage endpoint (`wham/usage`) is undocumented and could change or break without notice** → Mitigation: mark it "stable" in adapter metadata but keep the HTTP call isolated in one small function with a clear error path (falls back to "no usage data" rather than crashing the daemon), matching how the current Claude adapter already degrades on API errors.
- **[Risk] Users expect Kiro/Cursor/Gemini/Antigravity support since the daemon is now "multi-provider"** → Mitigation: README documents, per unsupported tool, the concrete technical reason it isn't included yet (see tasks.md docs section), not just "not supported."
- **[Risk] Linux/Windows may hit their own platform-specific BLE gaps** (unconfirmed) → Mitigation: tasks 1.3/1.4 spike each platform for real before assuming the macOS result generalizes; a helper-process fallback (Decision 2's superseded alternative) remains available per-platform if one of them turns out to need it.

## Migration Plan

1. Ship the new binary alongside the old Python daemon for one release (don't delete `daemon/*.py` / `*.sh` until the new binary is confirmed working on all three OSes in real use).
2. Existing users on the old install: `install-mac.sh`/`install.sh` v2 detects and unloads/disables the old LaunchAgent/systemd unit before installing the new binary's autostart entry, so there's no double-polling.
3. Config migration is a no-op (Decision 3) — old config files work unchanged.
4. Rollback: keep the old Python scripts in the repo (not deleted) for at least one release cycle so a user can `install-mac.sh --legacy` (or equivalent) if the Rust binary regresses for them.

## Open Questions

- Does btleplug's released (unpatched) BlueZ backend actually see already-connected peripherals on Linux, as research suggested, or does it need its own patch like macOS did? Task 1.3 must confirm this against real hardware, not assume it from the research summary.
- Does Windows (WinRT) need the same kind of already-connected-peripheral patch macOS did? The PR's `Devices_Enumeration` feature addition hints yes, but this is unconfirmed — task 1.4.
- Should we proactively push to get PR #437 merged upstream (or open our own cleaner PR) rather than depend on a personal fork branch indefinitely? Worth doing early given how load-bearing this capability is.
- Should the Windows tray app's UX be preserved via a lightweight rewrite, or is a plain background process acceptable for v1?
- (Deferred to a future change, not blocking this one) Is there a maintained, scriptable way to invoke Kiro's `/usage` non-interactively? Worth a spike whenever Kiro support is proposed.
