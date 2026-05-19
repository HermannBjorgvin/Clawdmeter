<#
    Wrapper launched by Task Scheduler at user logon.
    Activates the daemon venv and runs claude_usage_daemon.py, redirecting
    stdout/stderr to a rotating log file in %LOCALAPPDATA%\Clawdmeter\.
    Equivalent of the LaunchAgent stdout/stderr plist entries on macOS.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$DaemonDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Python    = Join-Path $DaemonDir '.venv\Scripts\python.exe'
$Script    = Join-Path $DaemonDir 'claude_usage_daemon.py'

$LogDir = Join-Path $env:LOCALAPPDATA 'Clawdmeter'
New-Item -ItemType Directory -Path $LogDir -Force | Out-Null

$LogOut = Join-Path $LogDir 'daemon.out.log'
$LogErr = Join-Path $LogDir 'daemon.err.log'

# Trim logs above 5 MB so they don't grow unbounded.
foreach ($f in @($LogOut, $LogErr)) {
    if ((Test-Path $f) -and (Get-Item $f).Length -gt 5MB) {
        Move-Item $f "$f.1" -Force
    }
}

if (-not (Test-Path $Python)) {
    "[$(Get-Date -Format 's')] venv missing at $Python -- run install-win.ps1 first" |
        Out-File -FilePath $LogErr -Append -Encoding utf8
    exit 1
}

& $Python -u $Script *>> $LogOut 2>> $LogErr
exit $LASTEXITCODE
