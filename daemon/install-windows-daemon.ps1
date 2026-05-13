$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$launcherPath = Join-Path $scriptDir "start-claude-usage-daemon.ps1"
$daemonPath = Join-Path $scriptDir "claude-usage-daemon.py"
$taskName = "Clawdmeter Claude Usage Daemon"
$configDir = Join-Path $env:USERPROFILE ".config\claude-usage-monitor"
$configPath = Join-Path $configDir "windows-daemon.json"

function Require-Command {
    param([string]$Name)

    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $cmd) {
        throw "Required command '$Name' was not found on PATH."
    }
    return $cmd.Source
}

function Ensure-ConfigFile {
    New-Item -ItemType Directory -Force -Path $configDir | Out-Null

    if (-not (Test-Path $configPath)) {
        @'
{
  "device_mac": "",
  "poll_interval": 60,
  "forget_device_on_scan_fail": false
}
'@ | Set-Content -Path $configPath -Encoding UTF8
    }
}

function Test-BleakInstalled {
    $probe = @'
import importlib.util, sys
sys.exit(0 if importlib.util.find_spec("bleak") else 1)
'@
    $probe | py -3 - | Out-Null
    return $LASTEXITCODE -eq 0
}

Write-Host "=== Clawdmeter Windows Daemon Install ==="
Write-Host ""

Write-Host "[1/4] Checking files..."
if (-not (Test-Path $daemonPath)) {
    throw "Daemon file not found: $daemonPath"
}
if (-not (Test-Path $launcherPath)) {
    throw "Launcher file not found: $launcherPath"
}
Write-Host "  Found daemon and launcher"
Write-Host ""

Write-Host "[2/4] Checking Python..."
$py = Require-Command "py"
$powershell = Require-Command "powershell"
Write-Host "  py launcher: $py"
Write-Host "  powershell: $powershell"
if (-not (Test-BleakInstalled)) {
    throw "Python package 'bleak' is not installed for py -3. Run: py -3 -m pip install bleak"
}
Write-Host "  bleak is installed"
Write-Host ""

Write-Host "[3/4] Creating config..."
Ensure-ConfigFile
Write-Host "  Config: $configPath"
Write-Host ""

Write-Host "[4/4] Registering scheduled task..."
$action = New-ScheduledTaskAction `
    -Execute $powershell `
    -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$launcherPath`""
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$settings = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries `
    -StartWhenAvailable `
    -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive -RunLevel Limited

Register-ScheduledTask `
    -TaskName $taskName `
    -Action $action `
    -Trigger $trigger `
    -Settings $settings `
    -Principal $principal `
    -Force | Out-Null

Start-ScheduledTask -TaskName $taskName

Write-Host ""
Write-Host "=== Done ==="
Write-Host ""
Write-Host "Scheduled task: $taskName"
Write-Host "Config file: $configPath"
Write-Host ""
Write-Host "Optional config values:"
Write-Host '  "device_mac": "A4:F0:0F:60:98:16"'
Write-Host '  "poll_interval": 60'
Write-Host '  "forget_device_on_scan_fail": true'
Write-Host ""
Write-Host "Useful commands:"
Write-Host "  Get-ScheduledTask -TaskName '$taskName'"
Write-Host "  Start-ScheduledTask -TaskName '$taskName'"
Write-Host "  Stop-ScheduledTask -TaskName '$taskName'"
Write-Host "  Unregister-ScheduledTask -TaskName '$taskName' -Confirm:`$false"
