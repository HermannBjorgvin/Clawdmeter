$ErrorActionPreference = "Stop"

$taskName = "Clawdmeter Claude Usage Daemon"

if (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue) {
    Stop-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
    Write-Host "Removed scheduled task: $taskName"
}
else {
    Write-Host "Scheduled task not found: $taskName"
}
