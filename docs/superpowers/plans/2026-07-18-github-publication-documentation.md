# GitHub Publication and Documentation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Publish the physically validated Clawdmeter fork under `Atzingen/Clawdmeter` with accurate English and Portuguese documentation for Claude Code, Codex, Activity, USB serial, and the ESP32-2432S024C.

**Architecture:** Keep the existing implementation history unchanged and document it on the verified `codex/multi-screen-carousel` branch. Publish that commit lineage as `esp32-2432s024c-codex` and make it the fork's default branch, while preserving the upstream-derived `main` branch because `upstream/main` currently contains three unvalidated commits not present in the device-tested lineage.

**Tech Stack:** Markdown, Git/GitHub CLI, PowerShell, Python/pytest, PlatformIO, GitHub-rendered repository assets.

## Global Constraints

- The public target is a GitHub fork named `Atzingen/Clawdmeter`.
- `origin` must point to `Atzingen/Clawdmeter`; `upstream` must point to `HermannBjorgvin/Clawdmeter`.
- Do not merge, rebase, cherry-pick, or force-push the three current `upstream/main` commits into the physically validated lineage.
- Publish local `codex/multi-screen-carousel` as remote branch `esp32-2432s024c-codex` and make it the fork's default branch.
- Do not open a pull request against the upstream project.
- Keep `README.md` English-first and create `docs/README.pt-BR.md` for the tested Portuguese workflow.
- Credit the original Clawdmeter project before describing fork-specific work.
- Preserve the inherited licensing gray-area warning; do not add a new license.
- Do not claim fixed board prices, official OpenAI/Anthropic endorsement, or a stable public Codex telemetry API.
- Do not modify firmware, daemon behavior, credentials, logs, or the running COM3/tray setup.
- Reuse existing repository images; do not fabricate a final-device screenshot.
- Validate every relative Markdown link and externally cited URL before publishing.

---

### Task 1: Reorganize the English README

**Files:**
- Modify: `README.md`
- Reference: `docs/superpowers/specs/2026-07-18-github-publication-documentation-design.md`
- Reference: `daemon/README-windows.md`
- Reference: `firmware/platformio.ini`
- Reference: `daemon/dashboard_collectors.py`
- Reference: `daemon/dashboard_payload.py`

**Interfaces:**
- Consumes: implemented environment names `esp32_2432s024c` and `esp32_2432s024c_landscape`, existing asset paths, and local collector semantics.
- Produces: the public English entrypoint linked by the Portuguese guide and rendered as the fork homepage.

- [ ] **Step 1: Replace the opening with explicit fork attribution and language navigation**

Use this meaning and link structure at the top, without claiming upstream endorsement:

```markdown
# Clawdmeter — Claude Code + Codex on a low-cost ESP32 display

This repository is a hardware and Windows-focused fork of
[Hermann Bjorgvin's Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter).
The original project created the ESP32 Claude Code usage dashboard and Clawd
pixel-art experience. This fork adds Codex and combined activity pages, a tested
USB-serial Windows service, and support for the capacitive 2.4-inch
ESP32-2432S024C in portrait and landscape.

[Guia em Português](docs/README.pt-BR.md)
```

- [ ] **Step 2: Add the visual gallery using only existing assets**

Render `assets/demo.gif`, `screenshots/splash.png`, and
`screenshots/amoled_18/splash.png`. Label inherited AMOLED screenshots as
examples from the original project and do not describe them as photographs of
the ESP32-2432S024C.

- [ ] **Step 3: Add the exact fork comparison table**

The table must cover provider data, pages, navigation, Windows transport,
hardware, and Activity. Include these concrete fork behaviors:

```markdown
- Claude, Codex, and Activity pages
- left-half tap goes back; right-half tap goes forward
- automatic forward cycle every 12 seconds
- manual navigation pauses auto-cycling for 30 seconds
- Claude Open/Busy/Waiting and Codex Unread with freshness state
- USB serial on Windows; Bluetooth is optional for this board
```

- [ ] **Step 4: Document the low-cost board and honest trade-offs**

Use the exact model `ESP32-2432S024C`, suffix `C` for capacitive touch, display
size 2.4 inches, resolution 240x320, and the links below:

```markdown
- Technical reference: https://circuitpython.org/board/sunton_esp32_2432S024C/
- Exact-model retail example: https://www.amazon.com/dp/B0CLGD2DG6
- Marketplace search: https://www.aliexpress.com/w/wholesale-esp32--2432s024c.html
- Upstream Waveshare comparison: https://www.waveshare.com/product/esp32-s3-touch-amoled-2.16.htm
```

Explain that the TFT/classic-ESP32 design is commonly cheaper, but offers lower
resolution and fewer integrated peripherals than the Waveshare AMOLED board.
Tell buyers to verify the exact suffix because `R` variants use resistive touch.

- [ ] **Step 5: Add the tested Windows quick start**

The quick start must distinguish one-time flashing from everyday operation:

```powershell
git clone https://github.com/Atzingen/Clawdmeter.git
cd Clawdmeter
git switch esp32-2432s024c-codex
powershell -ExecutionPolicy Bypass -File install-windows.ps1
```

For an already flashed board, state that Python 3.11+, the repository, the
installer/tray, and a USB data cable are sufficient; PlatformIO is only needed
to build or flash firmware.

Document both build environments:

```powershell
pio run -d firmware -e esp32_2432s024c -j 1
pio run -d firmware -e esp32_2432s024c_landscape -j 1
```

Use the existing upload command for portrait and an equivalent explicit
landscape command. State that the tested landscape orientation has USB on the
left.

- [ ] **Step 6: Explain data flow and limitations accurately**

Document that the Windows tray polls Claude usage, aggregates local Claude and
Codex session state, and sends compact aggregate JSON over USB approximately
every 60 seconds. Include these paths:

```text
%USERPROFILE%\.claude\sessions
%USERPROFILE%\.codex\sessions
%USERPROFILE%\.codex\.codex-global-state.json
```

State that Codex local JSON is an internal format, not a public supported API;
schema mismatch yields `Unavailable` without breaking Claude data. Link
`https://github.com/openai/codex` for product context without implying
endorsement.

- [ ] **Step 7: Preserve inherited platform, porting, asset, credit, and warning material**

Keep the useful macOS/Linux/BLE, porting, fonts, icons, splash tooling, Credits,
and Licensing gray area sections. Remove obsolete statements that the dashboard
has only two screens or only Claude data. Ensure the original project and Clawd
animation source remain credited.

- [ ] **Step 8: Run the README local-link check**

Run this PowerShell/Python check from the repository root:

```powershell
@'
from pathlib import Path
import re

root = Path.cwd()
text = (root / "README.md").read_text(encoding="utf-8")
targets = re.findall(r"!?\[[^\]]*\]\(([^)]+)\)", text)
missing = []
for target in targets:
    clean = target.split("#", 1)[0]
    if not clean or "://" in clean or clean.startswith("mailto:"):
        continue
    if not (root / clean).exists():
        missing.append(target)
assert not missing, f"Missing README targets: {missing}"
print("README local links: PASS")
'@ | python -
```

Expected: `README local links: PASS`.

- [ ] **Step 9: Commit the English README**

```powershell
git add -- README.md
git commit -m "docs: explain Codex dashboard fork"
```

---

### Task 2: Add the Portuguese ESP32-2432S024C Guide

**Files:**
- Create: `docs/README.pt-BR.md`
- Modify: `README.md`
- Reference: `daemon/README-windows.md`
- Reference: `install-windows.ps1`

**Interfaces:**
- Consumes: English README terminology, validated Windows commands, and the exact fork branch name.
- Produces: a self-contained Portuguese guide linked from the repository homepage.

- [ ] **Step 1: Write origin, scope, and differences in Portuguese**

Start with `# Clawdmeter com Claude Code, Codex e ESP32-2432S024C` and link the
English README plus original repository. Explicitly correct the project name to
Clawdmeter and state that this is a community fork.

- [ ] **Step 2: Add a Portuguese hardware checklist**

List the exact capacitive board, USB data cable, Windows 10/11, Python 3.11+,
Claude Code login, Codex with local sessions, and optional PlatformIO for
flashing. Repeat the stable technical/retail/search links and the TFT-versus-
AMOLED trade-off without fixed prices.

- [ ] **Step 3: Document installation on a new PC**

Use this sequence:

```powershell
git clone https://github.com/Atzingen/Clawdmeter.git
cd Clawdmeter
git switch esp32-2432s024c-codex
powershell -ExecutionPolicy Bypass -File install-windows.ps1
```

Explain that moving only the board is insufficient: each PC needs the tray
installed and running. Explain that an already flashed board does not need to be
flashed again.

- [ ] **Step 4: Document daily use and all three pages**

Describe Claude, Codex, and Activity; left/right tap behavior; 12-second cycle;
30-second holdoff; freshness; and `Unavailable`. State that the same USB cable
powers the display and transports data.

- [ ] **Step 5: Add build, flash, and troubleshooting commands**

Include both environment names, explicit COM-port examples, landscape USB-left
orientation, tray log path `%LOCALAPPDATA%\Clawdmeter\daemon.log`, and fixes for
USB cable, wrong COM port, expired Claude login, missing Codex sessions, and
orientation mismatch.

- [ ] **Step 6: Add privacy and compatibility notes**

State that only aggregates are sent to the display; session contents are not
sent. Warn that Codex local JSON is internal and may change. Link the official
OpenAI Codex repository and OpenAI brand guidelines for the Blossom mark.

- [ ] **Step 7: Validate all local links in both documents**

Adapt the Task 1 script to iterate over `README.md` and `docs/README.pt-BR.md`,
resolving relative targets from each document's parent directory.

Expected: `Markdown local links: PASS`.

- [ ] **Step 8: Commit the Portuguese guide**

```powershell
git add -- README.md docs/README.pt-BR.md
git commit -m "docs: add Portuguese setup guide"
```

---

### Task 3: Verify Documentation and Repository Integrity

**Files:**
- Verify: `README.md`
- Verify: `docs/README.pt-BR.md`
- Verify: `firmware/platformio.ini`
- Verify: `install-windows.ps1`
- Verify: `daemon/dashboard_collectors.py`

**Interfaces:**
- Consumes: completed English and Portuguese documentation.
- Produces: evidence that commands, links, tests, and build environments remain valid before publication.

- [ ] **Step 1: Scan for unfinished markers, obsolete claims, secrets, and local machine paths**

```powershell
rg -n -i "TB[D]|TO[D]O|FIXM[E]|Cloud Mirror|Cloud Code|only two screens|only Claude" README.md docs/README.pt-BR.md
rg -n "Gustavo|C:\\Users|gho_|sk-|accessToken|credentials\.json.*\{" README.md docs/README.pt-BR.md
```

Expected: no unintended hits; generic `%USERPROFILE%` references are allowed.

- [ ] **Step 2: Confirm implementation facts against the source**

```powershell
rg -n "esp32_2432s024c|esp32_2432s024c_landscape" firmware/platformio.ini
rg -n "POLL_INTERVAL|\.codex-global-state|sessions|token_count|unread" daemon
rg -n "12000|30000|DASHBOARD_PAGE_COUNT|DASHBOARD_CLAUDE|DASHBOARD_CODEX|DASHBOARD_ACTIVITY" firmware/src
```

Expected: both environments, collector paths, timers, and exactly three pages
match the documentation.

- [ ] **Step 3: Validate external links with redirects enabled**

Check these URLs and require HTTP 200 or an expected marketplace redirect:

```text
https://github.com/HermannBjorgvin/Clawdmeter
https://github.com/openai/codex
https://openai.com/brand/
https://circuitpython.org/board/sunton_esp32_2432S024C/
https://www.amazon.com/dp/B0CLGD2DG6
https://www.aliexpress.com/w/wholesale-esp32--2432s024c.html
https://www.waveshare.com/product/esp32-s3-touch-amoled-2.16.htm
```

If a marketplace blocks automated requests, verify that the browser-visible
page/search is correct and document that exception rather than deleting the
link.

- [ ] **Step 4: Run the complete Python suite**

```powershell
python -m pytest -q
```

Expected: at least the previously verified `158 passed, 2 skipped`; known
warnings may remain, but no failures are allowed.

- [ ] **Step 5: Compile both documented firmware environments serially**

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -d firmware -e esp32_2432s024c -j 1
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -d firmware -e esp32_2432s024c_landscape -j 1
```

Expected: both commands exit 0. Do not upload or touch COM3.

- [ ] **Step 6: Confirm clean diff and commit any documentation-only corrections**

```powershell
git diff --check
git status -sb
```

If verification required a documentation correction, stage only `README.md` and
`docs/README.pt-BR.md`, then commit `docs: correct setup instructions`.

---

### Task 4: Create the Fork and Publish the Verified Default Branch

**Files:**
- External state: GitHub repository `Atzingen/Clawdmeter`
- Git configuration: local remotes for this checkout

**Interfaces:**
- Consumes: clean verified HEAD from Task 3 and authenticated `gh` account `Atzingen`.
- Produces: public GitHub fork with `esp32-2432s024c-codex` as the default branch and working documentation URLs.

- [ ] **Step 1: Reconfirm authentication, scope, HEAD, and divergence**

```powershell
gh auth status
git status -sb
git remote -v
git rev-list --left-right --count upstream/main...HEAD
```

Expected: authenticated as `Atzingen`, clean worktree, current branch
`codex/multi-screen-carousel`, and upstream divergence recorded. Do not merge the
three upstream-only commits.

- [ ] **Step 2: Create the public fork and add it as `origin`**

```powershell
gh repo fork HermannBjorgvin/Clawdmeter --remote --remote-name origin --fork-name Clawdmeter
```

If the fork already exists, add or correct the remote instead:

```powershell
git remote add origin git@github.com:Atzingen/Clawdmeter.git
git remote set-url origin git@github.com:Atzingen/Clawdmeter.git
```

Expected: `gh repo view Atzingen/Clawdmeter --json isFork,parent,url` reports a
public fork whose parent is `HermannBjorgvin/Clawdmeter`.

- [ ] **Step 3: Normalize remote URLs**

```powershell
git remote set-url upstream https://github.com/HermannBjorgvin/Clawdmeter.git
git remote set-url origin git@github.com:Atzingen/Clawdmeter.git
git remote -v
```

Expected: fetch/push for `origin` target `Atzingen`; `upstream` targets Hermann.
No push may target `upstream`.

- [ ] **Step 4: Push the verified lineage without rewriting upstream main**

```powershell
git push -u origin HEAD:esp32-2432s024c-codex
```

Expected: push succeeds without `--force`.

- [ ] **Step 5: Make the verified branch the fork default and set metadata**

```powershell
gh repo edit Atzingen/Clawdmeter --default-branch esp32-2432s024c-codex --description "Clawdmeter fork with Claude Code, Codex, USB serial, and ESP32-2432S024C support"
```

Expected: `gh repo view Atzingen/Clawdmeter --json defaultBranchRef,description,url`
returns `esp32-2432s024c-codex` and the intended description.

- [ ] **Step 6: Verify the published GitHub pages**

Require HTTP 200 for:

```text
https://github.com/Atzingen/Clawdmeter
https://github.com/Atzingen/Clawdmeter/blob/esp32-2432s024c-codex/README.md
https://github.com/Atzingen/Clawdmeter/blob/esp32-2432s024c-codex/docs/README.pt-BR.md
```

Also use `gh api repos/Atzingen/Clawdmeter/contents/README.md?ref=esp32-2432s024c-codex`
and the equivalent Portuguese path to confirm the exact published SHA.

- [ ] **Step 7: Final handoff**

Report the fork URL, default branch, final commit, validation results, and that
no upstream pull request or force-push occurred. Leave the physical firmware and
tray untouched.
