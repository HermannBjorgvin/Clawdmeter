## ADDED Requirements

### Requirement: Provider adapter interface
The system SHALL define a single `UsageProvider` interface that every supported AI tool implements, consisting of a stable identifier, a support tier, a credential-discovery function, and a usage-polling function that returns a normalized payload. Only the `stable` tier is used in this change. Shared daemon logic (polling loop, active-provider selection, BLE payload assembly) SHALL depend only on this interface and MUST NOT branch on a specific provider's identity.

#### Scenario: Adding a provider does not modify shared code
- **WHEN** a new provider adapter satisfying the `UsageProvider` interface is registered
- **THEN** the polling loop, active-provider selection logic, and BLE payload assembly run unmodified against the new provider

#### Scenario: Provider-specific errors stay isolated
- **WHEN** one provider's `pollUsage` call throws or returns malformed data
- **THEN** the daemon logs the error, treats that provider as having no usable data for the current cycle, and continues polling the other enabled providers without crashing

### Requirement: Claude Code provider (stable)
The system SHALL include a `claude` adapter that discovers the OAuth access token from the macOS Keychain (service `Claude Code-credentials`) or the `<config_dir>/.credentials.json` file on Linux/Windows, and polls `api.anthropic.com/v1/messages` for rate-limit headers, matching the current Python daemon's behavior for a single config dir and for multiple `config_dirs` entries.

#### Scenario: macOS Keychain token is preferred when no credentials file exists
- **WHEN** the default Claude config dir has no `.credentials.json` file
- **THEN** the adapter reads the token from the macOS Keychain service `Claude Code-credentials`

#### Scenario: A present credentials file takes precedence over Keychain
- **WHEN** `<config_dir>/.credentials.json` exists and contains a valid `accessToken`
- **THEN** the adapter uses that file's token instead of querying the Keychain, even on macOS

#### Scenario: Multiple Claude config dirs are each polled
- **WHEN** the `config_dirs` option lists more than one directory
- **THEN** the adapter polls each directory's token independently and returns one normalized-usage result per directory

### Requirement: OpenAI Codex CLI provider (stable)
The system SHALL include a `codex` adapter that reads the access token from `~/.codex/auth.json` and polls `https://chatgpt.com/backend-api/wham/usage` with that token to obtain usage data, normalized into the same payload shape as the Claude provider.

#### Scenario: Missing Codex credentials disables the adapter for the cycle
- **WHEN** `~/.codex/auth.json` does not exist or contains no usable token
- **THEN** the codex adapter reports no usage data for that poll cycle without raising an unhandled error

### Requirement: Unsupported providers are documented, not silently ignored
The system SHALL NOT include adapters for Cursor, Kiro, Gemini CLI, or Google Antigravity in this change, and SHALL reject an attempt to enable a provider identifier that has no registered adapter with a clear configuration error rather than silently skipping it. The README SHALL document, per unsupported tool, the specific technical reason it isn't included (e.g. no local usage API, interactive-only CLI, org-scoped credential) rather than a generic "not supported" note.

#### Scenario: Enabling an unimplemented provider fails loudly
- **WHEN** the config's `providers` list contains an identifier with no registered adapter (e.g. `cursor` or `kiro`)
- **THEN** the daemon fails to start (or logs a clear startup error, per implementation) naming the unsupported provider, instead of silently ignoring it

#### Scenario: README explains each unsupported provider individually
- **WHEN** a reader checks the README for why Cursor, Kiro, Gemini CLI, or Antigravity usage isn't shown
- **THEN** they find a distinct, technically specific reason for that tool, not a single blanket statement covering all of them

### Requirement: Active-provider selection across multiple sources
When more than one provider (or more than one config dir within a provider) yields usage data in the same poll cycle, the system SHALL select the "active" source using the same recency heuristic as the current multi-config-dir logic: the source whose session usage percentage most recently increased is shown; ties keep the previously active source.

#### Scenario: Most recently active source wins
- **WHEN** two enabled providers both return usage data in the same cycle, and only one shows a session-usage increase since the last cycle
- **THEN** the daemon reports the payload from the provider that showed the increase

#### Scenario: No new activity keeps the previous selection
- **WHEN** neither enabled provider shows a session-usage increase since the last cycle
- **THEN** the daemon continues reporting the previously active provider's data
