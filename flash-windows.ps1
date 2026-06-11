# Build and flash Clawdmeter firmware on Windows.
# Usage:
#   .\flash-windows.ps1 <board>         # auto-detects ESP32 COM port
#   .\flash-windows.ps1 <board> COM14   # explicit port
#
# <board> is the PlatformIO env name, e.g. waveshare_amoled_216_c6
param(
    [Parameter(Position=0)]
    [string]$Board,
    [Parameter(Position=1)]
    [string]$Port
)

# esptool uses Unicode progress chars (▓░) that break on CP1250 terminals
chcp 65001 | Out-Null
$env:PYTHONUTF8 = "1"

$PIO = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$FirmwareDir = Join-Path $ScriptDir "firmware"

if (-not $Board) {
    Write-Host "Error: board env name is required." -ForegroundColor Red
    Write-Host "Usage: .\flash-windows.ps1 <board> [port]"
    Write-Host "Available boards:"
    Select-String -Path "$FirmwareDir\platformio.ini" -Pattern '^\[env:' |
        ForEach-Object { "  " + ($_.Line -replace '^\[env:', '' -replace '\]', '') }
    exit 1
}

if (-not $Port) {
    $detected = Get-PnpDevice -Class Ports -Status OK |
        Where-Object { $_.FriendlyName -like '*USB*' -and $_.FriendlyName -notlike '*Bluetooth*' } |
        Select-Object -First 1
    if ($detected) {
        if ($detected.FriendlyName -match 'COM\d+') { $Port = $Matches[0] } else { $Port = $null }
    }
    if (-not $Port) {
        Write-Host "Could not auto-detect USB serial port. Specify it manually:" -ForegroundColor Yellow
        Write-Host "  .\flash-windows.ps1 $Board COM14"
        exit 1
    }
    Write-Host "Auto-detected port: $Port" -ForegroundColor Cyan
}

Write-Host "=== Flashing Clawdmeter ===" -ForegroundColor Green
Write-Host "Board: $Board"
Write-Host "Port:  $Port"
Write-Host ""

Set-Location $FirmwareDir
& $PIO run -e $Board -t upload --upload-port $Port

Write-Host ""
Write-Host "=== Done! ===" -ForegroundColor Green
