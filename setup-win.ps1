<#
.SYNOPSIS
    First-time setup wizard for Clawdmeter on Windows.
.DESCRIPTION
    Walks through every post-purchase step in order, polling for hardware
    state where it can and prompting the user where it can't (BLE pairing
    in Windows Settings is the one step that cannot be automated):

      1. PlatformIO present (or offer install)
      2. ESP32 board detected on a USB COM port
      3. Firmware flash via flash-win.ps1
      4. Bluetooth pairing in Windows Settings
      5. Daemon install via install-win.ps1
      6. Verify the daemon writes its first usage payload to the board

    Each step is idempotent and can be re-entered if the wizard is
    interrupted partway through. Use -Step N to jump straight to step N
    (useful when re-running after fixing one specific failure).
.EXAMPLE
    .\setup-win.ps1
    .\setup-win.ps1 -Step 4    # skip prereq/flash, jump to pairing
#>
[CmdletBinding()]
[Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSAvoidUsingWriteHost', '',
    Justification = 'Installer-style console output; Write-Output would emit to the pipeline.')]
param(
    [ValidateRange(1, 6)]
    [int]$Step = 1,
    [int]$BoardWaitSeconds = 60,
    [int]$PairingWaitSeconds = 180,
    [int]$PayloadWaitSeconds = 120
)

$ErrorActionPreference = 'Stop'

# Force UTF-8 stdio for child Python processes. On a cp1252 console (default
# on non-English Windows) PlatformIO + Python 3.14 crashes its output-reader
# thread on the first non-ASCII byte from esptool's progress output, which
# silently hangs the flash mid-erase. Setting these env vars is harmless on
# English consoles where stdout is already utf-8 or cp437.
if (-not $env:PYTHONIOENCODING) { $env:PYTHONIOENCODING = 'utf-8' }
if (-not $env:PYTHONUTF8)       { $env:PYTHONUTF8       = '1' }

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

function Write-Step {
    param([int]$N, [string]$Title)
    Write-Host ""
    Write-Host "=== [$N/6] $Title ===" -ForegroundColor Cyan
}

function Get-CommandPath {
    param([string]$Name)
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

# ---------- Step 1: PlatformIO ----------
function Step-PlatformIO {
    Write-Step 1 'PlatformIO check'

    $pio = Get-CommandPath 'pio'
    if (-not $pio) {
        $fallback = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\pio.exe'
        if (Test-Path $fallback) { $pio = $fallback }
    }

    if ($pio) {
        $ver = & $pio --version 2>&1
        Write-Host "  Found: $pio"
        Write-Host "  $ver"
        return
    }

    Write-Host "  PlatformIO not found. It's needed to compile + upload the ESP32 firmware."
    $py = Get-CommandPath 'py'
    if (-not $py) { $py = Get-CommandPath 'python' }
    if (-not $py) {
        throw "Python is also missing -- install Python 3.9+ first (winget install Python.Python.3.12), then re-run this wizard."
    }

    $ans = Read-Host "  Install PlatformIO now via 'pip install --user platformio'? [Y/n]"
    if ($ans -match '^[Nn]') {
        throw "PlatformIO required. Install manually, then re-run with -Step 1."
    }

    if ((Split-Path -Leaf $py) -eq 'py.exe') {
        & $py -3 -m pip install --user --quiet platformio
    } else {
        & $py -m pip install --user --quiet platformio
    }
    if ($LASTEXITCODE -ne 0) { throw "PlatformIO install failed (pip exit $LASTEXITCODE)" }

    $fallback = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\pio.exe'
    if (-not (Test-Path $fallback)) { throw "PlatformIO installed but pio.exe not found at $fallback" }
    Write-Host "  Installed: $fallback"
    Write-Host "  (flash-win.ps1 finds it automatically; no PATH change needed for this wizard.)"
}

# ---------- Step 2: Wait for board ----------
function Find-Esp32Port {
    # Match by USB VID, not by driver Caption strings. Caption is localized
    # (NL Windows shows "Serieel USB-apparaat (COM3)" instead of "USB Serial
    # Device") and Microsoft's generic usbser driver fills Manufacturer with
    # "Microsoft" instead of "Espressif", so the previous English-only
    # caption regex missed ESP32-S3 USB JTAG devices on non-English Windows.
    # VIDs covered:
    #   303A = Espressif Systems (ESP32-S3 USB JTAG, ESP32-S2 native CDC)
    #   10C4 = Silicon Labs (CP210x family)
    #   1A86 = WCH (CH340/CH341/CH9102)
    #   0403 = FTDI (FT232R/FT232H/FT231X)
    Get-CimInstance -ClassName Win32_PnPEntity |
        Where-Object {
            $_.PNPDeviceID -match 'VID_(303A|10C4|1A86|0403)' -and
            ($_.Caption -match '\(COM\d+\)' -or $_.Name -match '\(COM\d+\)')
        } |
        ForEach-Object {
            $label = if ($_.Caption -match '\(COM\d+\)') { $_.Caption } else { $_.Name }
            if ($label -match '\((COM\d+)\)') { $Matches[1] }
        } |
        Sort-Object -Unique |
        Select-Object -First 1
}

function Step-WaitBoard {
    param([int]$TimeoutSeconds)
    Write-Step 2 'Detect ESP32 board on USB'

    $port = Find-Esp32Port
    if ($port) {
        Write-Host "  Already detected: $port"
        return $port
    }

    Write-Host "  Plug the board in with a DATA-capable USB-C cable. Charge-only cables won't enumerate."
    Write-Host "  Waiting up to $TimeoutSeconds seconds..."
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 2
        $port = Find-Esp32Port
        if ($port) {
            Write-Host "  Detected: $port"
            return $port
        }
        Write-Host -NoNewline '.'
    }
    Write-Host ""
    throw "No ESP32-like COM port appeared within $TimeoutSeconds s. Check Device Manager > Ports for a warning triangle, or try a different USB cable. Then re-run with -Step 2."
}

# ---------- Step 3: Flash firmware ----------
function Step-Flash {
    Write-Step 3 'Flash firmware'
    $flash = Join-Path $ScriptDir 'flash-win.ps1'
    if (-not (Test-Path $flash)) { throw "flash-win.ps1 missing at $flash" }

    Write-Host "  First flash takes 3-5 minutes (PlatformIO downloads the ESP32 toolchain)."
    Write-Host "  Subsequent flashes are <30 seconds."
    Write-Host ""

    & $flash
    if ($LASTEXITCODE -ne 0) { throw "Flash failed (exit $LASTEXITCODE). Re-run with -Step 3 once the issue is resolved." }

    Write-Host ""
    Write-Host "  Flash done. The board should now show the Clawd splash on its AMOLED."
}

# ---------- Step 4: Bluetooth pairing ----------
function Test-ClaudeControllerPaired {
    # Look for a paired BT device whose FriendlyName matches the GATT advertised name.
    Get-PnpDevice -Class Bluetooth -ErrorAction SilentlyContinue |
        Where-Object {
            $_.FriendlyName -match 'Claude Controller' -and
            $_.Status -in @('OK','Unknown')
        } |
        Select-Object -First 1
}

function Step-Pair {
    param([int]$TimeoutSeconds)
    Write-Step 4 'Pair the board over Bluetooth'

    if (Test-ClaudeControllerPaired) {
        Write-Host "  Already paired."
        return
    }

    Write-Host "  Pairing has to be done in Windows Settings -- bleak's WinRT backend"
    Write-Host "  refuses to connect to unpaired LE devices."
    Write-Host ""
    Write-Host "  On the board:"
    Write-Host "    Press the middle (PWR) button until you reach the Bluetooth screen"
    Write-Host "    (showing MAC + 'Connectable')."
    Write-Host ""
    Write-Host "  In Windows Settings (opening now):"
    Write-Host "    Add device -> Bluetooth -> pick 'Claude Controller' -> Done."
    Write-Host ""

    Start-Process 'ms-settings:bluetooth' | Out-Null

    Write-Host "  Waiting up to $TimeoutSeconds seconds for the pairing to complete..."
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        if (Test-ClaudeControllerPaired) {
            Write-Host ""
            Write-Host "  Paired."
            return
        }
        Start-Sleep -Seconds 3
        Write-Host -NoNewline '.'
    }
    Write-Host ""
    throw "Pairing didn't complete within $TimeoutSeconds s. Re-run with -Step 4 once 'Claude Controller' shows as Paired in Settings."
}

# ---------- Step 5: Install daemon ----------
function Step-InstallDaemon {
    Write-Step 5 'Install / refresh the host daemon'
    $install = Join-Path $ScriptDir 'install-win.ps1'
    if (-not (Test-Path $install)) { throw "install-win.ps1 missing at $install" }

    & $install
    if ($LASTEXITCODE -ne 0) { throw "install-win.ps1 failed (exit $LASTEXITCODE)" }
}

# ---------- Step 6: Verify first payload ----------
function Step-VerifyPayload {
    param([int]$TimeoutSeconds)
    Write-Step 6 'Verify first usage payload reaches the board'

    $logOut = Join-Path $env:LOCALAPPDATA 'Clawdmeter\daemon.out.log'
    if (-not (Test-Path $logOut)) {
        Write-Host "  Log file doesn't exist yet -- the scheduled task may still be spinning up."
        Write-Host "  Waiting for it to appear..."
        $appearDeadline = (Get-Date).AddSeconds(15)
        while (((Get-Date) -lt $appearDeadline) -and -not (Test-Path $logOut)) {
            Start-Sleep -Seconds 1
        }
        if (-not (Test-Path $logOut)) {
            throw "Log file never appeared at $logOut. Check the scheduled task ran: Get-ScheduledTaskInfo -TaskName 'Clawdmeter Daemon'"
        }
    }

    Write-Host "  Tailing $logOut for up to $TimeoutSeconds seconds, looking for 'Sending:' line..."
    Write-Host "  (Press Ctrl+C if you want to bail early -- the daemon keeps running.)"
    Write-Host ""

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $offset = 0
    if (Test-Path $logOut) { $offset = (Get-Item $logOut).Length }
    $found = $false
    $connected = $false
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 750
        $size = (Get-Item $logOut).Length
        if ($size -le $offset) { continue }
        $bytes = New-Object byte[] ($size - $offset)
        $fs = [System.IO.File]::Open($logOut, 'Open', 'Read', 'ReadWrite')
        try {
            [void]$fs.Seek($offset, 'Begin')
            [void]$fs.Read($bytes, 0, $bytes.Length)
        } finally { $fs.Dispose() }
        $offset = $size
        $chunk = [System.Text.Encoding]::UTF8.GetString($bytes)
        foreach ($line in ($chunk -split "`r?`n" | Where-Object { $_ })) {
            Write-Host "    $line"
            if ($line -match 'Connected\b' -and -not $connected) {
                $connected = $true
            }
            if ($line -match 'Sending:\s+(\{.*\})') {
                Write-Host ""
                Write-Host "  First payload sent: $($Matches[1])"
                $found = $true
                break
            }
        }
        if ($found) { break }
    }

    if (-not $found) {
        Write-Host ""
        Write-Host "  Didn't see a 'Sending:' line in time. Last log entries above."
        Write-Host "  Common causes: device not yet paired, paired but not advertising,"
        Write-Host "  or the board is connected to a different host. See README troubleshooting."
        throw "First-payload verification timed out."
    }
}

# ---------- Main ----------
try {
    Write-Host ""
    Write-Host "=========================================="
    Write-Host "  Clawdmeter Windows first-time setup"
    Write-Host "=========================================="
    if ($Step -gt 1) { Write-Host "  Starting from step $Step (skipping earlier steps)." }

    if ($Step -le 1) { Step-PlatformIO }
    if ($Step -le 2) { [void](Step-WaitBoard -TimeoutSeconds $BoardWaitSeconds) }
    if ($Step -le 3) { Step-Flash }
    if ($Step -le 4) { Step-Pair -TimeoutSeconds $PairingWaitSeconds }
    if ($Step -le 5) { Step-InstallDaemon }
    if ($Step -le 6) { Step-VerifyPayload -TimeoutSeconds $PayloadWaitSeconds }

    Write-Host ""
    Write-Host "==========================================" -ForegroundColor Green
    Write-Host "  All 6 steps passed. Clawdmeter is live." -ForegroundColor Green
    Write-Host "==========================================" -ForegroundColor Green
    Write-Host ""
    Write-Host "On the board, press the middle (PWR) button until you see the Usage screen."
    Write-Host "Percentages should match the payload logged above."
    Write-Host ""
    Write-Host "Live logs:"
    Write-Host "  Get-Content `"$env:LOCALAPPDATA\Clawdmeter\daemon.out.log`" -Tail 30 -Wait"
}
catch {
    Write-Host ""
    Write-Host "Setup interrupted: $_" -ForegroundColor Red
    Write-Host "Resolve the issue, then re-run with: .\setup-win.ps1 -Step <N>" -ForegroundColor Red
    exit 1
}
