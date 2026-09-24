# Petmeter on a BUSY Bar

Coding-agent usage on a [BUSY Bar](https://busy.app) — a 72×16 RGB LED matrix — rotating through every quota your plans meter, each with its label and its mascot.

![Petmeter on a BUSY Bar](../screenshots/busybar/card-1.png)

Every screenshot here is the real device: captured with
[`tools/busybar_shot.py --skin`](../tools/busybar_shot.py), which reads the
panel over the HTTP API and renders it as LEDs inside the hardware, the same
way the bar's own web interface does.

## Every screen

The cards rotate every four seconds, each carrying its provider's mascot.

| | |
|---|---|
| ![Current](../screenshots/busybar/card-1.png) | **Current** — the 5-hour window |
| ![Weekly](../screenshots/busybar/card-2.png) | **Weekly** — the 7-day window |
| ![Fable](../screenshots/busybar/card-3.png) | **Fable** — a scoped model allowance, when the plan meters one |
| ![Codex](../screenshots/busybar/card-4.png) | **Codex**, with Codey — the same reading for the other provider |
| ![Reset credits](../screenshots/busybar/card-5.png) | **Resets** — the credit ledger: held solid, spent hollow |

And the states you hope not to see:

| | |
|---|---|
| ![No data](../screenshots/busybar/state-nodata.png) | **no data** — the daemon is there, the reading is not. The pet stays: the app is alive |
| ![No host](../screenshots/busybar/state-nohost.png) | **no host** — the daemon is unreachable. No pet, because the pet lives on the host |
| ![Paused](../screenshots/busybar/state-paused.png) | **paused** — the 2×2 badge at the top right, the only room there was for one |
| ![Toast](../screenshots/busybar/state-toast-running.png) | the **toast** on a press — the word replaces the caption for two seconds, then the device removes it itself |

This is a self-contained corner of [Petmeter](../README.md). The desk meter it belongs to is an ESP32 device; nothing here needs one. All it needs is the Petmeter daemon running on a host the bar can reach.

## Two ways in

|  | Push | Pull |
|---|---|---|
| **What** | the daemon draws on the bar every poll | an app on the bar fetches and draws |
| **Where** | [`daemon/sinks/busybar.py`](../daemon/sinks/busybar.py) | [`petmeter/`](petmeter) — a TypeScript app built with `busy-cli` |
| **Shows** | one quota | every quota, rotating, with mascots |
| **Needs** | a config line | installing the app and launching it |
| **Works today** | yes | yes — launch it from the apps menu, or the CLI |

They are complementary: push keeps the bar current while you are not looking; pull makes it right the moment you select it.

### Push

```ini
# ~/.config/claude-usage-monitor/config
busybar_url = http://10.0.4.20      # USB; or http://busybar.local over Wi-Fi
```

Restart the daemon. Dry-run without it:

```bash
python -m daemon.sinks.busybar http://10.0.4.20
```

### Pull

```ini
# ~/.config/claude-usage-monitor/config
busybar_serve = 10.0.4.21:8724      # this host, on the bar's USB network
```

```bash
cd busybar/petmeter && pnpm install && pnpm build
python3 ../../tools/busybar_install_app.py http://10.0.4.20
```

Then launch it over the bar's CLI (`telnet 10.0.4.20`, port 23):

```
js -i app.petmeter /ext/user_assets/app.petmeter/scripts/main.js
```

**Controls are written but cannot run yet.** They follow what the case is engraved with — the red **Start/Pause** bar holds and releases the rotation, the wheel **scrolls** by hand, and its press, labelled **OK/Skip**, skips forward — but a CLI-launched script gets no input API. Its globals are exactly `console`, `setInterval`, `setTimeout`, `clearInterval`, `clearTimeout`, `Request`, `fetch` and `localStorage`; `listen` is installed only for an app launched as an app, which is the same thing [the apps menu blocks](#why-it-is-not-in-the-apps-menu). The binding is guarded and logs that it is disabled, so the display keeps working.

## Nothing here is hosted

Both halves run on hardware you own: the daemon on your machine, the app on the
bar, talking over the USB link (`10.0.4.21` ↔ `10.0.4.20`). The pull endpoint
binds to that interface alone, not `0.0.0.0`.

There is one case where a server would help — reading your usage while away
from the machine — and it is the case to avoid. The numbers come from OAuth
tokens that Claude Code and the Codex CLI keep locally, and this project's rule
is that it only ever reads tokens it does not own. Relaying them through a
hosted service would mean shipping someone else's credentials off the laptop.

Distribution is a GitHub Release, not a deploy: `.github/workflows/release.yml`
turns a version tag into the `.tgz` device package, built by `busy-cli`.

## What we learned the hard way

Everything here was found against real hardware, and none of it is in the published API spec.

**The device and the cloud mount the same API at different prefixes.** `api.busy.app` documents `/busybar/...`; that is the cloud relay. The bar serves **`/api/...`**. Posting to the documented path reaches the bar's web-UI file server, which answers `405 Allow: GET` — an error that reads like "wrong method" and means "wrong prefix".

**Access over Wi-Fi is off by default; over USB it is not gated.** Ask the device rather than inferring: `GET /api/access` is ungated and returns `{"mode": "disabled"|"enabled"|"key"}`. Over USB the bar is always **10.0.4.20** (printed on its back cover) and the host is **10.0.4.21**.

**An out-of-memory abort is completely silent.** This one cost the most. The JS runtime's heap is small — 40,000 array pushes kills it — and when it dies, nothing is logged: the script's first line prints, the last never does, no error, no exit message. `@busy-app/busy-lib`'s `render()` ships font metric tables that exceed it, so the app composes its elements by hand with absolute coordinates. A 72×16 screen wants absolute positions anyway.

**Image paths resolve against the app root**, not the assets folder — `appmeta/assets/clawd_16.png`, not `clawd_16.png`. And one unreadable image **rejects the entire draw** with a 400, so a wrong path means no frame at all rather than a frame with a gap in it.

**`rectangle` has no `color`.** It has a `fill` (default `none`) and a border (default 1px, **white**). Pass a colour and nothing else and you get a white outline.

**Storage writes are create-only.** Writing over an existing file returns `508 "Failed to open file for writing"`, which reads like a disk fault and means "this path is taken". Delete first. A running app also holds its script open, so stop it before reinstalling — the installer does both.

**Panel greys vanish.** There is no lit background for them to sit against, so the track colour that reads as "secondary" on an AMOLED reads as "off" here.

**`GET /api/screen` is not the `image/bmp` it advertises.** The front panel returns **base64 text** decoding to 3456 bytes (72×16×3) in **BGR**; the back returns 6400 bytes of 8-bit greyscale at 80×80 for a 160×80 panel, so it is half width. [`tools/busybar_shot.py`](../tools/busybar_shot.py) handles both — QA the layout by looking at it, the way the firmware is QA'd.

**The bar is slow over Wi-Fi and goes quiet.** A draw answers in ~5.0s, consistently, and after a burst of requests it stops answering for ~20s. Over USB the same draw returns in under 0.1s. A timeout set at the measured response time is a coin flip, not a margin.

## Launching it, and the apps menu

**Launch it as a real app**, not as a loose script:

```
loader open js_app_launcher app.petmeter
```

over the device's telnet CLI (port 23). `js -i app.petmeter <path>` also runs
the script, but as a CLI job rather than an app.

**The apps menu is flag-gated, not hardcoded.** `apps_menu_is_js_apps_enabled()`
stats a file; without it the menu shows "More apps soon" and lists nothing,
which reads exactly like a firmware that cannot list user apps. It can, once
asked:

```bash
python3 tools/busybar_install_app.py http://10.0.4.20 --enable-menu
```

That writes `/ext/apps_data/apps_menu/js_apps_enabled`, and the placeholder is
replaced by the real list.

**A manifest key can stop the app loading.** `busy-cli` scaffolds
`heap_size_kib`, which firmware 1.2.4 does not know, and the launcher answers
`App loading failed, reinstall it.` The loader checks only three things —
`appmeta/manifest.json` parses, the directory name equals the manifest `id`,
and `scripts/main.js` exists — so a manifest it cannot parse fails all of it
with one message. Removing that key is the whole fix. Icons
(`appmeta/icon_front_8x8.png`, `icon_back_11x11.png`) are optional to load but
are what the menu shows.

## The buttons

The bar's own buttons work, but not from inside the app. `js_input.c` — the
file that installs the `listen` global — was committed on **2026-09-16**, five
days *after* release **1.2.4** (2026-09-11), which is what the device runs;
that path 404s at the tag. Its `js_runner.c` sets up exactly `console`, the
interval functions, `fetch` and `localStorage`, which is precisely what a
`for…in` probe enumerates on the hardware.

So the **host** reads them instead. The device's CLI (TCP 23) has an `input
dump` command that prints one line per physical event, and because it
subscribes to the same pubsub the GUI does, **it sees presses the canvas has
already swallowed** — which is every press while our elements are on screen.

That puts the control state on the host ([`serve.Control`](../daemon/sinks/serve.py)),
including the rotation clock, and makes the app a renderer:

```
button → input dump → daemon control state → gen++ → long poll returns → app draws
```

`GET /usage.json?since=<gen>&wait=8000` holds open until something changes, so
a press reaches the screen in one round trip rather than waiting out a polling
interval — and a return with nothing changed is not wasted, because redrawing
every 8s also restores the frame after anything else clears the canvas.

| | |
|---|---|
| red **Start/Pause** bar | holds and releases the rotation |
| wheel **scroll** | steps through the cards |
| wheel press (**OK/Skip**) | skips forward |

**macOS needs to allow it.** The daemon runs under launchd, and connecting out
to the bar's LAN address is gated by Local Network privacy — a background
daemon cannot show that prompt, so it is denied silently and the log reads
`no CLI (OSError: [Errno 65] No route to host)`. Grant the daemon's Python
binary Local Network access in System Settings → Privacy & Security.

**Push and pull cannot both run.** `canvas_draw_rejected` refuses a *different*
`application_name` at *equal* priority, so the sink (`petmeter`) and the app
(`app.petmeter`) fight for the screen at priority 50 — whichever drew first
wins and the other gets 409s. Set `busybar_url` or `busybar_serve`, not both.

## Layout

```
 0            23 26                                      71
├─── mascot ────┤├─ 17% (large, rows 0..8) ──── 3h40m ───┤
                ├─ Current ──────────────────────────────┤  rows 11..15
```

**The countdown sits beside the number, not beside the label.** "Current" and
"3h40m" want 52px of a 46px row, so one of them has to give: first the time
gave (`3h40` — a duration nobody writes, and the one number on the card worth
acting on), then the label gave (`Curre`). Both were wrong. The top row has
free space on every quota card, so the time went there and the label got the
caption row to itself. At 100% the percent sign gives way instead, so three
digits never reach the countdown.

**There is no bar.** In a 46px pane a `large` number cannot share a row with
any label, and label + 9px number + a bar do not stack in 16 rows — something
had to give. The bar was earning least: its track (`#2A2A28`) is invisible on
an LED matrix, so it never showed headroom, and its one real job, carrying the
alarm, a 9px number does better than a 2px stripe. **The number is the alarm
now** — white below 75%, amber to 90%, red above. White rather than green at
rest: with nothing else tinted, permanent green is wallpaper.

**The mascots.** Clawd is authored on a 12×8 grid at 100px a cell, so he
renders at exactly 2× — 24×16 — with no resampling. That matters: averaging
him down blends orange into transparency, and on an unlit matrix that blend
reads as *brown*, while his square eyes smear into diagonal marks. Codey is 16
wide, centred in the same slot; both stand 16 tall, which is what reads as the
same size.

**The reset time shortens rather than collides**: `1h25m → 1h25 → 1h`,
`5d21h → 5d`. Formatted in-app, never with the device's `countdown` element —
that renders `HH:MM:SS` in a wide font, ticks every 100 ms, and takes hours
modulo 60, so a five-day reset would read as 21 hours.

**Reset credits** keep the desk device's ledger beside the number: one cell per
credit the window handed out, solid while held, hollow once spent. Below 3px a
hollow cell has no hole left, so the cells are dropped rather than lie.

**A press gets a toast.** Pause and resume put the word over the caption row
for two seconds with `timeout: 2`, and the device deletes it itself — no second
request and no state to unwind.

**Every frame names every element id**, unused ones as tombstones. Draws merge
by id, so anything left unnamed stays on screen — including elements from an
older build, which is why the app clears its canvas once at startup.
