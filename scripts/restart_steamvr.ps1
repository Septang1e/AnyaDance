param(
    [string]$SteamExe
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "common_steamvr.ps1")

foreach ($Name in @("vrserver", "vrmonitor", "vrcompositor")) {
    Get-Process $Name -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}
Write-Host "Stopped SteamVR server processes." -ForegroundColor Yellow

$SteamExe = Resolve-SteamExe -SteamExe $SteamExe
Start-Process -FilePath $SteamExe -ArgumentList "steam://run/250820" -WindowStyle Hidden
Write-Host "Requested SteamVR start through Steam." -ForegroundColor Cyan
