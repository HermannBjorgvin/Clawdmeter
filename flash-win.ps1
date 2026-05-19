<#
.SYNOPSIS
    Build and flash Clawdmeter firmware on Windows.
.DESCRIPTION
    Mirrors flash-mac.sh. Auto-detects the ESP32-S3 USB JTAG/serial debug
    unit COM port via PnP queries; pass -Port to override.
.EXAMPLE
    .\flash-win.ps1
    .\flash-win.ps1 -Port COM7
#>
[CmdletBinding()]
[Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSAvoidUsingWriteHost', '',
    Justification = 'Installer-style console output; Write-Output would emit to the pipeline.')]
param(
    [string]$Port
)

$ErrorActionPreference = 'Stop'
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

function Resolve-Pio {
    $cmd = Get-Command pio -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $candidate = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\pio.exe'
    if (Test-Path $candidate) { return $candidate }
    throw "PlatformIO CLI not found. Install with: pip install --user platformio  (or use the VSCode PlatformIO extension and re-run this from its terminal)"
}

function Find-Esp32Port {
    $ports = Get-CimInstance -ClassName Win32_PnPEntity |
        Where-Object {
            $_.Caption -match 'COM\d+' -and (
                $_.Caption -match 'USB JTAG' -or
                $_.Caption -match 'USB Serial' -or
                $_.Caption -match 'CP210' -or
                $_.Caption -match 'CH340' -or
                $_.Manufacturer -match 'Espressif'
            )
        } |
        ForEach-Object {
            if ($_.Caption -match '\((COM\d+)\)') { $Matches[1] }
        } |
        Sort-Object -Unique

    if (-not $ports) { return $null }
    return $ports[0]
}

$pio = Resolve-Pio

if (-not $Port) {
    $Port = Find-Esp32Port
    if (-not $Port) {
        Write-Error "No ESP32-like COM port detected. Plug the board in via USB-C, or pass -Port COMx explicitly. Use 'pio device list' to see candidates."
        exit 1
    }
    Write-Host "Auto-detected port: $Port"
}

Write-Host "=== Flashing Clawdmeter ==="
Write-Host "Port: $Port"
Write-Host ""

Push-Location (Join-Path $ScriptDir 'firmware')
try {
    & $pio run -t upload --upload-port $Port
    if ($LASTEXITCODE -ne 0) { throw "pio upload failed (exit $LASTEXITCODE)" }
}
finally {
    Pop-Location
}

Write-Host ""
Write-Host "=== Done ==="
Write-Host "Monitor with: pio device monitor -p $Port -b 115200"
