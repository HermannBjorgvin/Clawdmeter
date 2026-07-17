# Windows Setup and Run Guide

This guide covers running the Clawdmeter Windows daemon on native Windows hardware.
It includes the turnkey `install-windows.ps1` bootstrap (tray icon + login autostart),
the manual-run fallback, and how to manage or remove autostart.

The tray application now uses **USB serial by default**. Keep the ESP32 connected to
the computer with a USB data cable; Bluetooth pairing is not required for displaying
usage. The original BLE daemon remains available as
`claude_usage_daemon_windows.py` for boards that need it.

---

## Prerequisites

| Requirement | Details |
|-------------|---------|
| **Native Windows** | Must run on real Windows — not WSL. |
| **Python 3.11+** | Download from [python.org](https://www.python.org/downloads/) if not already installed. Ensure "Add python.exe to PATH" is checked during install. |
| **Claude Code installed** | Install Claude Code and complete `claude login` so credentials exist on disk. |
| **Clawdmeter connected by USB** | Use a data-capable USB cable. The same cable powers the display and carries usage data. |

### Where are my credentials?

`claude login` writes the OAuth token to (first match wins):

1. `%USERPROFILE%\.claude\.credentials.json` — primary path (confirmed by Claude Code docs)
2. `%LOCALAPPDATA%\Claude\.credentials.json` — fallback
3. `%APPDATA%\Claude\.credentials.json` — fallback

The daemon probes these paths in order. You can also set `CLAUDE_CREDENTIALS_PATH` to an
absolute path or `CLAUDE_CONFIG_DIR` to a directory to override the search entirely.

> **Security note:** The credentials file contains your OAuth token. Never share its contents
> or embed it in scripts. The daemon reads it from disk and uses it only as the API
> `Authorization` header — the token is never written to any log, tooltip, or notification.

---

## Bluetooth pairing (optional)

The upstream Clawdmeter is a **bonded BLE HID keyboard** as well as a usage display. Pairing
is optional in the USB-serial setup and is only useful on boards whose buttons send
keyboard shortcuts. The ESP32-2432S024C usage display does not need this step.

For a BLE-based board, its firmware
enables bonding (`NimBLEDevice::setSecurityAuth`) and advertises the HID service so its
physical buttons act as a keyboard (Space / Shift+Tab). Pair it with Windows **once**,
before running the daemon:

1. Put the device on its Bluetooth waiting screen (powered on, not yet connected).
2. Open **Settings → Bluetooth & devices → Add device → Bluetooth**.
3. Select **Clawdmeter** and complete pairing.

**Why this is required:**

- **Keyboard buttons** — HID over BLE requires bonding on Windows. Without pairing, the
  device's buttons won't reach the PC.
- **Persistent point-in-time view** — once paired, Windows maintains the BLE link and
  auto-reconnects the device whenever it is in range. This is intentional: the device keeps
  showing your **last-synced** usage even after you Quit the daemon, as a glanceable
  point-in-time view. Quitting the daemon releases only its data connection — it does **not**
  drop the Windows pairing, so the device stays connected to Windows.

To undo, use **Settings → Bluetooth & devices → (device) → Remove device**. Removing the
pairing disables the keyboard buttons.

---

## Setup (one time)

Open a PowerShell terminal and `cd` to the repository root.

**1. Create a virtual environment**

```powershell
python -m venv .venv
```

**2. Activate it**

```powershell
.venv\Scripts\Activate.ps1
```

If you see a scripts-execution-policy error, run:
```powershell
Set-ExecutionPolicy -Scope CurrentUser -ExecutionPolicy RemoteSigned
```
Then repeat the `Activate.ps1` step.

**3. Install dependencies**

```powershell
pip install -r daemon\requirements-windows.txt
```

This installs `pyserial`, `httpx`, the tray dependencies, and the legacy BLE dependency.

---

## Running the daemon

With the venv active and the Clawdmeter powered on:

```powershell
python daemon\claude_usage_daemon_serial_windows.py
```

### ESP32-2432S024C orientation builds

```powershell
# Portrait
pio run -d firmware -e esp32_2432s024c -j 1

# Landscape
pio run -d firmware -e esp32_2432s024c_landscape -j 1
```

Use the landscape build with USB on the left. Each orientation requires its matching
touch transform, so keep the selected firmware environment and touch mapping paired.

### Expected console output

```
[HH:MM:SS] === Claude Usage Tracker Daemon (USB serial, Windows) ===
[HH:MM:SS] Clawdmeter identified on COM3
[HH:MM:SS] Sending by USB serial: {"s":42,"sr":180,"w":17,"wr":8820,"st":"active","ok":true}
```

- The daemon probes physical USB serial ports and asks each device to identify itself;
  Windows Bluetooth COM ports are ignored.
- Set `CLAWDMETER_SERIAL_PORT=COM3` to pin the port if automatic detection is undesirable.
- After identifying the device, the daemon polls the Anthropic API immediately and sends the first
  payload within a few seconds of connect (warm token path). With a valid, non-expired token
  the device should leave its waiting screen and show session + weekly percentages within
  about 10 seconds of launch.
- The daemon then re-polls every 60 seconds while connected.
- If the USB cable disconnects, the daemon releases the failed session and probes again with
  exponential backoff.

### Stopping

Press **Ctrl+C** in the terminal. The daemon logs `Daemon stopping` and exits cleanly.

---

## Dashboard data

- Claude rate limits still come from the existing authenticated poll.
- Claude Code activity reads only aggregate `status` fields from `.claude\sessions`.
- Codex usage reads aggregate `token_count` events and rate-limit windows from
  `.codex\sessions`.
- Codex unread count comes from `.codex-global-state.json`.
- `Waiting` means the Claude CLI status is `idle`; `Unread` does not necessarily mean
  a reply is required.
- Codex local JSON is an internal format. If its schema does not match, Codex values
  appear unavailable while Claude continues working.
- Prompt text, responses, paths, and task titles are never included in the serial payload.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| `Warning: running under Linux/WSL` | Running in WSL, not native Windows | Run from a native PowerShell or Command Prompt on Windows |
| `Scanning for 'Clawdmeter'… Device not found` | Clawdmeter is off, out of range, or not yet paired | Power on the device, pair it once (see [Pair the device](#pair-the-device-one-time)), and ensure it is in range |
| `No token; sending local dashboard data only` | No credentials file found at any candidate path | Confirm `claude login` ran on this machine; check `%USERPROFILE%\.claude\.credentials.json` exists |
| `API HTTP 401` | Token expired | Re-run `claude login` in a terminal to refresh the token, then restart the daemon |
| `Connection failed` | WinRT BLE initialisation issue | Ensure Windows Bluetooth is on; try toggling Bluetooth off/on in Windows Settings |

---

## Tray icon, login autostart, and turnkey install

### One-command install (recommended)

> **Copy the repo to a native Windows path first.** Clone or copy this repository
> to a Windows location such as `%USERPROFILE%\Clawdmeter` — **not** a WSL share
> (`\\wsl$\...` or `\\wsl.localhost\...`). Installing from the WSL share would point
> the virtual environment and the login-autostart entry at a path that disappears when
> WSL shuts down, defeating the whole point of the Windows daemon. The installer
> detects a WSL path and refuses to run, telling you how to relocate.
>
> ```powershell
> Copy-Item -Recurse '\\wsl.localhost\Ubuntu\home\<you>\repos\Clawdmeter' "$env:USERPROFILE\Clawdmeter"
> cd "$env:USERPROFILE\Clawdmeter"
> ```

Run this once from the repository root in PowerShell (a native Windows path):

```powershell
powershell -ExecutionPolicy Bypass -File install-windows.ps1
```

The script does four things in order and logs progress at each step:

1. Creates a Python virtual environment at `.venv`.
2. Installs dependencies from `daemon\requirements-windows.txt` (bleak, httpx, pystray, Pillow).
3. Registers the tray app to launch automatically at login via `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` — per-user, no admin required.
4. Launches the tray app immediately (headless — no console window).

The script downloads nothing from the internet. It only installs the packages listed in
the in-repo `daemon\requirements-windows.txt`.

### Tray icon and status

After install, the Clawdmeter icon appears in the Windows notification area:

| State | Icon bubble | Tooltip |
|-------|-------------|---------|
| Connected | green | `Connected · last update HH:MM` |
| Scanning | amber | `Scanning…` |
| Error | red | `Error: token expired — run claude login` |

Hover over the icon to see the current status tooltip. A notification fires once when the
daemon first enters the Error state (e.g. after a token expiry).

### Tray menu

Right-click the tray icon for the menu:

- **Status header** (non-clickable) — live status + last data sync time.
- **Start at login** (checkable toggle) — enables or disables autostart at runtime.
  Reflects the current registry state each time the menu opens.
- **Quit** — stops the daemon cleanly, releases the COM port, and exits with no lingering
  process. The device keeps showing the last-synced usage.

### Disabling or removing autostart

Use the tray menu toggle, or remove the registry value manually:

```powershell
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v Clawdmeter /f
```

### WSL independence

The daemon operates fully independently of WSL. The token is read from native Windows
credential paths (`%USERPROFILE%\.claude\.credentials.json` and fallbacks), and serial
communication uses the native Windows COM port. Running `wsl --shutdown` does not affect
the daemon.

---

## What is NOT covered here

- PyInstaller / one-file `.exe` packaging — v2
- MAC-address cache / sleep-wake reconnect hardening — Phase 3
