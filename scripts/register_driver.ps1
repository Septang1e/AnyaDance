param(
    [string]$SteamVRRoot,
    [string]$DriverRoot,
    [string]$SteamConfigPath,
    [string]$BackupPath,
    [switch]$ForceBackup
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "common_steamvr.ps1")

function Get-JsonSection {
    param([pscustomobject]$Object, [string]$Name)
    if (-not $Object.PSObject.Properties[$Name]) {
        $Object | Add-Member -NotePropertyName $Name -NotePropertyValue ([pscustomobject]@{})
    }
    return $Object.PSObject.Properties[$Name].Value
}

function Set-JsonProperty {
    param([pscustomobject]$Object, [string]$Name, $Value)
    if ($Object.PSObject.Properties[$Name]) { $Object.PSObject.Properties[$Name].Value = $Value }
    else { $Object | Add-Member -NotePropertyName $Name -NotePropertyValue $Value }
}

if (-not $DriverRoot) { $DriverRoot = Join-Path (Join-Path $PSScriptRoot "..") "build\out\anyadance" }
$DriverRoot = (Resolve-Path $DriverRoot).Path
$steamvr = Resolve-SteamVRRoot -SteamVRRoot $SteamVRRoot
$vrpathreg = $steamvr.VrPathReg
if (-not (Test-Path (Join-Path $DriverRoot "driver.vrdrivermanifest"))) {
    throw "DriverRoot does not contain driver.vrdrivermanifest: $DriverRoot. Run build_driver.ps1 first."
}

Write-Host "Registering AnyaDance at $DriverRoot" -ForegroundColor Cyan
Write-Host "Using SteamVR at $($steamvr.Root)" -ForegroundColor Cyan
& $vrpathreg removedriver $DriverRoot 2>$null | Out-Null
& $vrpathreg adddriver $DriverRoot
Write-Host "Current anyadance driver registration:" -ForegroundColor Cyan
& $vrpathreg finddriver anyadance

# Apply the fully-virtual SteamVR settings (virtual HMD, controllers, trackers).
if (-not $SteamConfigPath) { $SteamConfigPath = Resolve-SteamVrSettingsPath }
if (-not $BackupPath) { $BackupPath = Get-AnyaDanceBackupPath }
$backupDir = Split-Path -Parent $BackupPath
if (-not (Test-Path $backupDir)) { New-Item -ItemType Directory -Path $backupDir | Out-Null }

# Back up only when no backup exists yet, so repeated registration cannot
# overwrite the pristine settings with already-modified ones. Unregister
# restores and clears the backup.
if ((Test-Path $SteamConfigPath) -and ($ForceBackup -or -not (Test-Path $BackupPath))) {
    Copy-Item -LiteralPath $SteamConfigPath -Destination $BackupPath -Force
    Write-Host "Backed up SteamVR settings to $BackupPath" -ForegroundColor Cyan
}

if (Test-Path $SteamConfigPath) { $settings = Get-Content -LiteralPath $SteamConfigPath -Raw | ConvertFrom-Json }
else { $settings = [pscustomobject]@{} }

$steamvrSettings = Get-JsonSection -Object $settings -Name "steamvr"
$driver = Get-JsonSection -Object $settings -Name "driver_anyadance"
Set-JsonProperty -Object $driver -Name "enable" -Value $true
Set-JsonProperty -Object $driver -Name "enable_hmd" -Value $true
Set-JsonProperty -Object $driver -Name "enable_controllers" -Value $true
Set-JsonProperty -Object $driver -Name "enable_trackers" -Value $true
Set-JsonProperty -Object $steamvrSettings -Name "activateMultipleDrivers" -Value $true
Set-JsonProperty -Object $steamvrSettings -Name "forcedDriver" -Value "anyadance"
Set-JsonProperty -Object $steamvrSettings -Name "requireHmd" -Value $true

# Keep the virtual HMD display awake: a held-still virtual headset never triggers
# SteamVR's idle screen-off, so push the timeout out far and keep the compositor
# running through standby. Restored from the backup on unregister.
$power = Get-JsonSection -Object $settings -Name "power"
Set-JsonProperty -Object $power -Name "turnOffScreensTimeout" -Value 86400.0
Set-JsonProperty -Object $power -Name "pauseCompositorOnStandby" -Value $false

$settingsJson = $settings | ConvertTo-Json -Depth 32
Set-Content -LiteralPath $SteamConfigPath -Value $settingsJson -Encoding UTF8
Write-Host "Wrote AnyaDance fully-virtual settings to $SteamConfigPath" -ForegroundColor Green

# Record the exact path we registered (the AnyaDance AppData folder already exists
# from the backup step) so a later unregister can remove it even if the bundle is
# moved afterward.
Write-AnyaDanceTextFile -Path (Get-AnyaDanceRegisteredPathRecord) -Content $DriverRoot
Write-Host "Restart SteamVR for registration and startup setting changes to take effect." -ForegroundColor Yellow
