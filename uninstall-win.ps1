<#
.SYNOPSIS
    Remove the Clawdmeter scheduled task and (optionally) the venv.
.EXAMPLE
    .\uninstall-win.ps1                  # just unregister the task
    .\uninstall-win.ps1 -RemoveVenv      # also delete daemon\.venv
    .\uninstall-win.ps1 -RemoveLogs      # also delete %LOCALAPPDATA%\Clawdmeter
#>
[CmdletBinding()]
[Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSAvoidUsingWriteHost', '',
    Justification = 'Installer-style console output; Write-Output would emit to the pipeline.')]
param(
    [switch]$RemoveVenv,
    [switch]$RemoveLogs,
    [string]$TaskName = 'Clawdmeter Daemon'
)

$ErrorActionPreference = 'Stop'
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

$task = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
if ($task) {
    if ($task.State -eq 'Running') {
        Stop-ScheduledTask -TaskName $TaskName
    }
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
    Write-Host "Unregistered scheduled task: $TaskName"
} else {
    Write-Host "No scheduled task named '$TaskName' found."
}

if ($RemoveVenv) {
    $venv = Join-Path $ScriptDir 'daemon\.venv'
    if (Test-Path $venv) {
        Remove-Item -Recurse -Force $venv
        Write-Host "Removed venv: $venv"
    }
}

if ($RemoveLogs) {
    $logDir = Join-Path $env:LOCALAPPDATA 'Clawdmeter'
    if (Test-Path $logDir) {
        Remove-Item -Recurse -Force $logDir
        Write-Host "Removed logs: $logDir"
    }
    $addrCache = Join-Path $env:USERPROFILE '.config\claude-usage-monitor\ble-address'
    if (Test-Path $addrCache) {
        Remove-Item -Force $addrCache
        Write-Host "Removed BLE address cache: $addrCache"
    }
}

Write-Host ""
Write-Host "Note: BLE pairing in Windows Settings is not touched. Remove 'Claude Controller'"
Write-Host "manually under Settings -> Bluetooth & devices if you want a clean slate."
