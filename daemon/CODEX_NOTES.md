# Codex Usage Signal Notes

Phase 0 source of truth:

- Codex CLI version checked locally: `codex-cli 0.130.0`.
- Local ChatGPT auth is stored in `~/.codex/auth.json` with top-level `tokens`.
  Token values are intentionally omitted from this note.
- Official `openai/codex` source maps ChatGPT backend usage from
  `https://chatgpt.com/backend-api/wham/usage`. The adjacent
  `https://chatgpt.com/backend-api/codex/usage` path returned the same payload
  in local measurement.
- The usage payload has top-level `rate_limit.primary_window` and
  `rate_limit.secondary_window`. Each window includes `used_percent`,
  `limit_window_seconds`, `reset_after_seconds`, and `reset_at`.
- Codex TUI displays percent left by calculating `100 - used_percent`. The BLE
  schema wants used percent, so Clawdmeter sends `used_percent` directly.
- Mapping:
  - `rate_limit.primary_window.used_percent` -> BLE `s`
  - `primary_window.reset_at` or `reset_after_seconds` -> BLE `sr` in minutes
  - `rate_limit.secondary_window.used_percent` -> BLE `w`
  - `secondary_window.reset_at` or `reset_after_seconds` -> BLE `wr` in minutes
  - `rate_limit.allowed == true` -> BLE `st = "allowed"`
- 401 recovery follows Codex CLI's ChatGPT refresh flow:
  `POST https://auth.openai.com/oauth/token` with JSON
  `client_id = app_EMoamEEZ73f0CkXaXp7hrann`, `grant_type = refresh_token`, and
  the stored refresh token. Refreshed tokens are written back to the same auth
  store.
