# Attribution & notices

## Origin

This project began as a fork of **[HermannBjorgvin/Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter)**
by Hermann Björgvin. The original is a desk-side Claude Code usage monitor for
Waveshare AMOLED ESP32 boards. This repository extends it into a multi-provider
monitor (Claude, OpenAI Codex, Antigravity/Gemini) with per-provider usage tabs,
a `/stats`-style screen, an activity heatmap, and a host-resource view.

It is published with fresh git history; full credit for the original firmware,
board HAL, splash engine, and BLE daemon design belongs to the upstream author.

## Third-party & proprietary assets — read before reusing

The upstream project bundles assets it does not own a redistribution license
for, and this fork inherits and adds to that. Specifically:

- **Anthropic proprietary fonts** (Tiempos, Styrene) compiled into `firmware/src/font_*.c`.
- **Anthropic's "Clawd" mascot** and brand palette (logo, splash pixel-art).
- **OpenAI** and **Google Gemini** logos/marks (`firmware/src/*_logo.h`,
  `assets/`), used as provider identifiers.

These belong to their respective owners and are included here for personal,
non-commercial use only. No license is granted to them. If you fork or copy this
repository, you are responsible for your own use of these assets — the upstream
author's warning applies and is amplified here. See the README's licensing note.

## Own code

The code written for this project (daemon logic, firmware UI, tests) carries no
proprietary claim, but is not separately licensed because the repository as a
whole contains the third-party assets above.
