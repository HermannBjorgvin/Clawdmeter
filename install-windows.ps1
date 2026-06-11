# install-windows.ps1 - Clawdmeter Windows turnkey bootstrap (D-09)
#
# Creates a Python virtual environment, installs dependencies from
# daemon\requirements-windows.txt, registers the tray app to launch at login
# (HKCU\...\Run, no admin required), and starts the tray app immediately.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File install-windows.ps1
#
# Or, if you have already set a permissive execution policy:
#   .\install-windows.ps1
#
# To disable autostart later: right-click the tray icon -> uncheck "Start at login"
# Or remove manually: reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v Clawdmeter /f
#
# Security: this script downloads nothing from the internet. It installs only
# the packages listed in the in-repo daemon\requirements-windows.txt.

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Log {
    param([string]$Msg)
    $ts = Get-Date -Format "HH:mm:ss"
    Write-Host "[$ts] $Msg"
}

$RepoRoot = $PSScriptRoot
if (-not $RepoRoot) {
    $RepoRoot = (Get-Location).Path
}

Log "=== Clawdmeter Windows Install ==="
Log "Repository root: $RepoRoot"

# ------------------------------------------------------------------
# Guard: refuse to install from a WSL path (APP-02 / SC#4 / SC#5)
# ------------------------------------------------------------------
# If $RepoRoot lives on the WSL share (\\wsl$\... or \\wsl.localhost\...),
# the venv and the HKCU\Run autostart entry would both point at a path that
# disappears when WSL is shut down -- exactly the WSL-dependence this project
# exists to eliminate. Copy the repo to a native Windows path first.
if ($RepoRoot -match '\\\\wsl(\$|\.localhost)\\') {
    throw @"
Refusing to install from a WSL path:
  $RepoRoot

The Clawdmeter daemon must be WSL-independent. Installing from the WSL share
would make the virtual environment and login-autostart entry point at a path
that is unreachable once WSL shuts down.

Fix: copy this repository to a native Windows location and run the installer
there, e.g.

  Copy-Item -Recurse '$RepoRoot' "$env:USERPROFILE\Clawdmeter"
  cd "$env:USERPROFILE\Clawdmeter"
  powershell -ExecutionPolicy Bypass -File install-windows.ps1
"@
}

# ------------------------------------------------------------------
# Step 1: Create virtual environment
# ------------------------------------------------------------------
$VenvDir = Join-Path $RepoRoot ".venv"
if (Test-Path $VenvDir) {
    Log "Virtual environment already exists at .venv - skipping creation"
} else {
    Log "Creating virtual environment at .venv ..."
    & python -m venv $VenvDir
    if ($LASTEXITCODE -ne 0) { throw "Failed to create virtual environment (exit $LASTEXITCODE)" }
    Log "Virtual environment created"
}

# ------------------------------------------------------------------
# Step 2: Install dependencies
# ------------------------------------------------------------------
$PythonExe  = Join-Path $VenvDir "Scripts\python.exe"
$PythonwExe = Join-Path $VenvDir "Scripts\pythonw.exe"
$RequirementsFile = Join-Path $RepoRoot "daemon\requirements-windows.txt"

Log "Installing dependencies from daemon\requirements-windows.txt ..."
& $PythonExe -m pip install --quiet -r $RequirementsFile
if ($LASTEXITCODE -ne 0) { throw "pip install failed (exit $LASTEXITCODE)" }
Log "Dependencies installed"

# ------------------------------------------------------------------
# Step 3: Register autostart (HKCU\Run, per-user, no admin needed)
# ------------------------------------------------------------------
# Derive all paths at install time - never hard-code an absolute path that
# breaks when the repository is moved (CLAUDE.md "repoint ExecStart" lesson,
# RESEARCH Anti-Pattern).
$TrayScript = Join-Path $RepoRoot "daemon\tray_windows.py"

Log "Registering autostart (HKCU\Software\Microsoft\Windows\CurrentVersion\Run) ..."
# Invoke the autostart helper via the just-created venv python so sys.executable
# resolves to the venv's pythonw.exe (the path that will be written to the registry).
& $PythonExe -c @"
import sys, os
sys.path.insert(0, r'$RepoRoot')
import daemon.autostart_windows as a
a.enable(tray_script=r'$TrayScript')
"@
if ($LASTEXITCODE -ne 0) { throw "Autostart registration failed (exit $LASTEXITCODE)" }
Log "Autostart registered - Clawdmeter will launch automatically at next logon"

# ------------------------------------------------------------------
# Step 4: Launch the tray app (headless - BASE pythonw.exe, no console window)
# ------------------------------------------------------------------
# Use the BASE interpreter's pythonw.exe, NOT the venv's Scripts\pythonw.exe.
# The venv pythonw is a redirector stub that re-launches the CONSOLE python.exe
# build as a child (a CPython venv-launcher bug), popping a black console window.
# tray_windows.py adds the venv site-packages to sys.path itself, so the venv's
# dependencies still resolve. (See autostart_windows._command - same rationale.)
$BasePrefix  = & $PythonExe -c "import sys; print(sys.base_exec_prefix)"
$BasePythonw = Join-Path $BasePrefix "pythonw.exe"

Log "Launching tray app ..."
$StartArgs = @{
    FilePath         = $BasePythonw
    ArgumentList     = "`"$TrayScript`""
    WorkingDirectory = $RepoRoot
}
Start-Process @StartArgs
Log "Tray app started - look for the Clawdmeter icon in your notification area"
Log "=== Install complete ==="
