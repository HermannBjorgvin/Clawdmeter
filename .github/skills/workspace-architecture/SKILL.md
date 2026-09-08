---
name: workspace-architecture
description: "Use when mapping Clawdmeter architecture, dependencies, protected contracts, generated files, or the impact of a firmware, daemon, UI, or asset change."
---

# Workspace Architecture

1. Check `git status --short`; preserve unrelated user changes.
2. Query `graphify-out/graph.json` with `graphify query` when available.
3. Read `AGENTS.md`, `CLAUDE.md`, and the nearest authoritative documentation.
4. Identify source files, generated files, runtime boundaries, and applicable instructions.
5. Report a bounded impact map: files, contracts, owner, first validation check, and documentation impact.

Treat these as protected boundaries unless explicitly requested: BLE UUIDs and characteristics, daemon payload keys, 135x240 display assumptions, and generated asset headers.
