$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$daemonPath = Join-Path $scriptDir "claude-usage-daemon.py"
$configDir = Join-Path $env:USERPROFILE ".config\claude-usage-monitor"
$configPath = Join-Path $configDir "windows-daemon.json"
$logPath = Join-Path $configDir "daemon-launcher.log"

function Write-LauncherLog {
    param([string]$Message)

    $timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    $line = "[$timestamp] $Message"
    New-Item -ItemType Directory -Force -Path $configDir | Out-Null
    Add-Content -Path $logPath -Value $line
}

function Resolve-PythonExecutable {
    $commands = @(
        @{ Command = "py"; Args = @("-3") },
        @{ Command = "python"; Args = @() }
    )

    foreach ($candidate in $commands) {
        $resolved = Get-Command $candidate.Command -ErrorAction SilentlyContinue
        if ($resolved) {
            return @{
                Path = $resolved.Source
                Args = $candidate.Args
            }
        }
    }

    throw "Python 3 was not found on PATH."
}

function Get-ExistingDaemonProcess {
    $escapedDaemonPath = [Regex]::Escape($daemonPath)
    Get-CimInstance Win32_Process |
        Where-Object {
            $_.Name -match '^pythonw?(\.exe)?$' -and
            $_.CommandLine -match $escapedDaemonPath
        } |
        Select-Object -First 1
}

try {
    $existing = Get-ExistingDaemonProcess
    if ($existing) {
        Write-LauncherLog "Daemon already running with PID $($existing.ProcessId); launcher will exit."
        exit 0
    }

    if (Test-Path $configPath) {
        $config = Get-Content -Raw -Path $configPath | ConvertFrom-Json
        if ($null -ne $config.device_mac -and $config.device_mac -ne "") {
            $env:DEVICE_MAC = [string]$config.device_mac
        }
        if ($null -ne $config.poll_interval -and $config.poll_interval -ne "") {
            $env:POLL_INTERVAL = [string]$config.poll_interval
        }
        if ($null -ne $config.forget_device_on_scan_fail) {
            $env:FORGET_DEVICE_ON_SCAN_FAIL = if ([bool]$config.forget_device_on_scan_fail) { "1" } else { "0" }
        }
    }

    $python = Resolve-PythonExecutable
    $arguments = @()
    $arguments += $python.Args
    $arguments += @($daemonPath)

    Write-LauncherLog "Starting daemon via $($python.Path) $($arguments -join ' ')"

    $process = Start-Process `
        -FilePath $python.Path `
        -ArgumentList $arguments `
        -WorkingDirectory $scriptDir `
        -WindowStyle Hidden `
        -PassThru

    Write-LauncherLog "Daemon started with PID $($process.Id)"
}
catch {
    Write-LauncherLog "Launcher failed: $($_.Exception.Message)"
    throw
}
