---
applyTo: "firmware/**,daemon/**,tools/**,screenshot.sh"
description: "Use for build gates, payload fixtures, serial screenshot checks, and release validation."
---

# Validation Rules

- Define one cheap discriminating check before editing.
- Run the narrowest executable check immediately after each substantive edit.
- Firmware changes require a PlatformIO build; UI changes additionally require screenshots when hardware is available.
- Protocol changes require representative payload parsing or transport fixtures.
- Asset changes require regeneration and a firmware size check.
- Record unavailable hardware checks and remaining risk instead of implying they passed.
