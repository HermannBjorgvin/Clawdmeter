---
name: hunting-clawd-animations
description: Finds and archives new official Anthropic Clawd animations from claude.ai and the Claude desktop app - name-probing claude.ai/images with fetch.sh, grepping a fresh desktop .deb's ion-dist bundles, and recovering Lottie sources - then classifies and adds a find to the converter. Use when asked to look for new, seasonal or unreleased Clawd animations, refresh research/clawd-official/, or add an official animation to the device.
---

# Hunting Clawd animations

All official Clawd art lives as plain files under
`https://claude.ai/images/clawd/{core,persona}/…`. Static assets are not
Cloudflare-gated (only HTML routes are), so `curl` with a browser
User-Agent works. The archive and inventory are in `research/clawd-official/`
(`README.md` = inventory, `CLAUDE.md` = format facts and classification).

## Sources, in order of yield

1. **Name probing.** The image server answers HTTP 200 for every name: a real
   GIF for a valid one, an HTML catch-all otherwise. Check the magic bytes
   (`GIF8`). Seven current animations (Soccer, Basketball, Skateboard,
   Trumpet, Walking, Jumping, Pointing) are referenced by no shipped bundle
   and were found this way; Anthropic stages seasonal drops (Soccer appeared
   for the World Cup). Extend the `PROBE` list in
   `research/clawd-official/fetch.sh` with candidate names (seasons, sports,
   instruments, hobbies) and run it:

   ```bash
   research/clawd-official/fetch.sh     # re-downloads KNOWN assets, then probes; prints NEW HIT lines
   ```

2. **The Linux desktop .deb**, downloaded without installing. Its
   `usr/lib/claude-desktop/resources/ion-dist/` is a local, greppable mirror
   of the claude.ai web bundles:

   ```bash
   curl -s https://downloads.claude.ai/claude-desktop/apt/stable/dists/stable/main/binary-amd64/Packages | grep ^Filename
   # the last line is the newest; download https://downloads.claude.ai/claude-desktop/apt/stable/<that path>, then:
   dpkg-deb -x claude-desktop_*.deb deb/
   grep -rhoE '/images/[A-Za-z0-9/_.-]+\.(gif|png|webp)' deb/usr/lib/claude-desktop/resources/ion-dist/ | sort -u
   grep -rl '"nm":"Clawd-' deb/usr/lib/claude-desktop/resources/ion-dist/   # embedded Lottie JSON
   ```

   Work in a scratch dir, not the repo. `app.asar` (`npx @electron/asar extract`)
   holds the Electron shell but no Clawd art.

3. **claude.ai/code bundles** in a user-captured browser HAR reference the
   same `/images/clawd/…` paths; the Laptop Lottie came from
   `https://assets-proxy.anthropic.com/claude-ai/v2/assets/v1/<hash>.json`.

4. **GitHub archaeology**: `companion-inc/pet-clawd` (earlier archive with
   usage notes), `Mapleeeeeeeeeee/digest-showcase` (preserved the
   Clawd-Soccer Lottie). `anthropics/claude-desktop-buddy` is the official
   BLE maker API, a separate thread.

## Adding a find

```
New animation:
- [ ] Downloaded into research/clawd-official/, magic bytes checked
- [ ] Added to KNOWN in fetch.sh and to README.md's inventory (frames, timing, source)
- [ ] Classified per research/clawd-official/CLAUDE.md: loop, scene (idle → prop → loop → idle) or empty-stage
- [ ] Prefer a Lottie source over a rescaled, antialiased GIF when one exists
- [ ] Entry added to ANIMS in tools/convert_official_clawd.js
- [ ] Header regenerated and checked with --verify PNGs (regenerating-firmware-assets skill)
- [ ] Shown in the simulator at 480×480 and 240×240 (verifying-firmware-ui skill)
```

Only official art goes on the device; never draw or retouch Clawd frames.
