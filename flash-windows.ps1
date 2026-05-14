# flash-windows.ps1 — Build and flash Clawdmeter firmware on Windows.
# Usage:
#   .\flash-windows.ps1           # auto-detect COM port
#   .\flash-windows.ps1 COM3      # explicit port
param(
    [string]$Port
)
$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not $Port) {
    # Auto-detect ESP32 USB-JTAG or CDC-ACM serial port via WMI device names.
    $candidates = Get-WmiObject Win32_PnPEntity |
        Where-Object { $_.Name -match "USB JTAG|USB Serial Device|CP210|CH340|CH341|FTDI" } |
        ForEach-Object { if ($_.Name -match "(COM\d+)") { $Matches[1] } }
    if ($candidates) {
        $Port = $candidates | Select-Object -First 1
        Write-Host "Auto-detected port: $Port"
    } else {
        Write-Error "No ESP32 serial port found. Plug in via USB-C and specify the port: .\flash-windows.ps1 COM3"
    }
}

# Locate pio: prefer PATH, then default PlatformIO install location.
$pio = (Get-Command pio -ErrorAction SilentlyContinue)?.Source
if (-not $pio) {
    $pio = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\pio.exe"
    if (-not (Test-Path $pio)) {
        Write-Error "'pio' not found. Install PlatformIO CLI: https://platformio.org/install/cli"
    }
}

Write-Host "=== Flashing Clawdmeter ==="
Write-Host "Port: $Port"
Write-Host ""

Set-Location (Join-Path $ScriptDir "firmware")
& $pio run -t upload --upload-port $Port

Write-Host ""
Write-Host "=== Done ==="
Write-Host "Monitor with: pio device monitor -p $Port -b 115200"
