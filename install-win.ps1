<#
.SYNOPSIS
    Windows installer for the Clawdmeter daemon.
.DESCRIPTION
    Mirrors install-mac.sh and install.sh but uses a Python venv + Task
    Scheduler "At log on" trigger instead of LaunchAgents / systemd.

    Steps:
      1. Verify Python + credentials file
      2. Create daemon\.venv and install pyserial + httpx
      3. Register the scheduled task "Clawdmeter Daemon" (hidden, current user)
      4. Start the task

.EXAMPLE
    .\install-win.ps1
    .\install-win.ps1 -SkipScheduledTask    # venv + deps only, no autostart
#>
[CmdletBinding()]
[Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSAvoidUsingWriteHost', '',
    Justification = 'Installer-style console output; Write-Output would emit to the pipeline.')]
param(
    [switch]$SkipScheduledTask,
    [string]$TaskName = 'Clawdmeter Daemon'
)

$ErrorActionPreference = 'Stop'

# Force UTF-8 stdio for child Python processes. Mirrors the same hardening in
# flash-win.ps1: on a cp1252 console (default on non-English Windows) any
# non-ASCII byte from a child pip/python subprocess can crash that process's
# stdout-encoder thread. Harmless on English consoles.
if (-not $env:PYTHONIOENCODING) { $env:PYTHONIOENCODING = 'utf-8' }
if (-not $env:PYTHONUTF8)       { $env:PYTHONUTF8       = '1' }

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$DaemonDir = Join-Path $ScriptDir 'daemon'
$VenvDir   = Join-Path $DaemonDir '.venv'
$Wrapper   = Join-Path $DaemonDir 'run-daemon.ps1'
$DaemonPy  = Join-Path $DaemonDir 'claude_usage_daemon.py'

Write-Host "=== Clawdmeter Windows install ==="
Write-Host ""

# --- [1/4] Prerequisites ---
Write-Host "[1/4] Checking prerequisites..."

function Resolve-Python {
    # Resolve to the actual python.exe path (not the 'py' launcher wrapper),
    # so we don't have to juggle the launcher's '-3' arg through PowerShell's
    # call operator on every invocation.
    foreach ($candidate in @('py', 'python3', 'python')) {
        $cmd = Get-Command $candidate -ErrorAction SilentlyContinue
        if (-not $cmd) { continue }
        try {
            if ($candidate -eq 'py') {
                $exe = (& $cmd.Source -3 -c "import sys; print(sys.executable)") 2>$null
            } else {
                $exe = (& $cmd.Source -c "import sys; print(sys.executable)") 2>$null
            }
            if (-not $exe -or -not (Test-Path $exe)) { continue }
            $verOut = & $exe --version 2>&1
            if ($verOut -match 'Python\s+3\.(\d+)') {
                if ([int]$Matches[1] -ge 9) { return $exe }
            }
        } catch {
            Write-Verbose "Skipping candidate '$candidate': $_"
        }
    }
    return $null
}

$Python = Resolve-Python
if (-not $Python) {
    throw "Python 3.9+ not found. Install from https://python.org or 'winget install Python.Python.3.12'."
}
Write-Host "  Python: $Python"

$credsPath = Join-Path $env:USERPROFILE '.claude\.credentials.json'
if (-not (Test-Path $credsPath)) {
    Write-Warning "  $credsPath not found. Sign in via 'claude' (Claude Code CLI) first, or the daemon will idle until you do."
} else {
    Write-Host "  Claude credentials: present"
}
Write-Host ""

# --- [2/4] Virtualenv ---
Write-Host "[2/4] Creating Python virtualenv at daemon\.venv ..."
if (-not (Test-Path $VenvDir)) {
    & $Python -m venv $VenvDir
    if ($LASTEXITCODE -ne 0) { throw "venv creation failed" }
}
$VenvPython = Join-Path $VenvDir 'Scripts\python.exe'
if (-not (Test-Path $VenvPython)) { throw "venv python missing at $VenvPython" }

& $VenvPython -m pip install --quiet --upgrade pip
if ($LASTEXITCODE -ne 0) { throw "pip upgrade failed" }
& $VenvPython -m pip install --quiet 'pyserial>=3.5' 'httpx>=0.27'
if ($LASTEXITCODE -ne 0) { throw "pip install failed" }
Write-Host "  OK ($VenvPython)"
Write-Host ""

# --- [3/4] Smoke import ---
Write-Host "[3/4] Verifying imports..."
$smokeCmd = @'
import importlib.metadata as m, importlib.util, sys, pathlib
print("pyserial", m.version("pyserial"), "| httpx", m.version("httpx"))
# Also import the daemon module to catch syntax / API issues against the installed Python.
spec = importlib.util.spec_from_file_location("daemon_under_test", pathlib.Path(sys.argv[1]))
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
print("daemon imports clean against", sys.version.split()[0])
'@
& $VenvPython -c $smokeCmd $DaemonPy
if ($LASTEXITCODE -ne 0) { throw "Import smoke test failed" }
Write-Host ""

# --- [4/4] Scheduled task ---
if ($SkipScheduledTask) {
    Write-Host "[4/4] Skipping Task Scheduler registration (per -SkipScheduledTask)."
    Write-Host "      Run manually with:"
    Write-Host "        powershell -ExecutionPolicy Bypass -File `"$Wrapper`""
    Write-Host ""
} else {
    Write-Host "[4/4] Registering scheduled task '$TaskName'..."

    # Remove any prior instance so re-running is idempotent.
    if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
        Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
    }

    $action = New-ScheduledTaskAction `
        -Execute 'powershell.exe' `
        -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$Wrapper`""

    $trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME

    $settings = New-ScheduledTaskSettingsSet `
        -AllowStartIfOnBatteries `
        -DontStopIfGoingOnBatteries `
        -StartWhenAvailable `
        -ExecutionTimeLimit ([TimeSpan]::Zero) `
        -RestartInterval (New-TimeSpan -Minutes 1) `
        -RestartCount 3

    # Run hidden as current user, no admin elevation required.
    $principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Limited

    Register-ScheduledTask `
        -TaskName $TaskName `
        -Action $action `
        -Trigger $trigger `
        -Settings $settings `
        -Principal $principal `
        -Description 'Polls Claude Code usage every 60s and pushes to Clawdmeter ESP32 over BLE.' | Out-Null

    Start-ScheduledTask -TaskName $TaskName
    Write-Host "  Registered and started: $TaskName"
    Write-Host ""
}

Write-Host "=== Done ==="
Write-Host ""
Write-Host "First-time Bluetooth pairing (after firmware is flashed):"
Write-Host "  1. Power on the Clawdmeter."
Write-Host "  2. Open Settings -> Bluetooth & devices -> Add device -> Bluetooth."
Write-Host "  3. Pair 'Claude Controller'."
Write-Host "  4. Within ~30 s the daemon will discover it and start polling."
Write-Host ""
Write-Host "Useful commands:"
Write-Host "  Get-ScheduledTask -TaskName '$TaskName'           # is it registered?"
Write-Host "  Start-ScheduledTask  -TaskName '$TaskName'        # start now"
Write-Host "  Stop-ScheduledTask   -TaskName '$TaskName'        # stop"
Write-Host "  Get-Content `"$env:LOCALAPPDATA\Clawdmeter\daemon.out.log`" -Tail 30 -Wait"
Write-Host "  Get-Content `"$env:LOCALAPPDATA\Clawdmeter\daemon.err.log`" -Tail 30 -Wait"
