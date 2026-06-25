function Get-SteamInstallRoots {
    $roots = @()
    foreach ($RegistryPath in @(
        "HKCU:\Software\Valve\Steam",
        "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam"
    )) {
        try {
            $InstallPath = (Get-ItemProperty -Path $RegistryPath -ErrorAction Stop).SteamPath
            if ($InstallPath) { $roots += [string]$InstallPath }
        } catch {
            continue
        }
    }
    $roots | Select-Object -Unique
}

function Get-SteamLibraryRoots {
    param([string[]]$SteamRoots)

    $libraries = @()
    foreach ($SteamRoot in $SteamRoots) {
        if (-not $SteamRoot) { continue }
        $libraries += $SteamRoot
        $LibraryFolders = Join-Path $SteamRoot "steamapps\libraryfolders.vdf"
        if (-not (Test-Path $LibraryFolders)) { continue }
        $Text = Get-Content -LiteralPath $LibraryFolders -Raw
        foreach ($Match in [regex]::Matches($Text, '"path"\s+"([^"]+)"')) {
            $libraries += ($Match.Groups[1].Value -replace "\\\\", "\")
        }
        foreach ($Match in [regex]::Matches($Text, '"\d+"\s+"([^"]+)"')) {
            $libraries += ($Match.Groups[1].Value -replace "\\\\", "\")
        }
    }
    $libraries | Select-Object -Unique
}

function Resolve-SteamVRRoot {
    param([string]$SteamVRRoot)

    $checked = New-Object System.Collections.Generic.List[string]
    $candidates = @()
    if ($SteamVRRoot) { $candidates += $SteamVRRoot }
    if ($env:ANYADANCE_STEAMVR_ROOT) { $candidates += $env:ANYADANCE_STEAMVR_ROOT }
    if ($env:STEAMVR_ROOT) { $candidates += $env:STEAMVR_ROOT }

    $steamRoots = @(Get-SteamInstallRoots)
    $libraries = @(Get-SteamLibraryRoots -SteamRoots $steamRoots)
    foreach ($Library in $libraries) {
        $candidates += (Join-Path $Library "steamapps\common\SteamVR")
    }
    $candidates += "C:\Program Files (x86)\Steam\steamapps\common\SteamVR"

    foreach ($Candidate in ($candidates | Where-Object { $_ } | Select-Object -Unique)) {
        $vrpathreg = Join-Path $Candidate "bin\win64\vrpathreg.exe"
        $checked.Add($Candidate)
        if (Test-Path $vrpathreg) {
            return [pscustomobject]@{
                Root = (Resolve-Path $Candidate).Path
                VrPathReg = (Resolve-Path $vrpathreg).Path
                Checked = @($checked)
            }
        }
    }

    $message = "Could not find SteamVR vrpathreg.exe.`nChecked:"
    foreach ($Path in $checked) { $message += "`n  - $Path" }
    $message += "`nPass -SteamVRRoot or set ANYADANCE_STEAMVR_ROOT."
    throw $message
}

function Resolve-SteamVrSettingsPath {
    $openvrPaths = Join-Path $env:LOCALAPPDATA "openvr\openvrpaths.vrpath"
    if (Test-Path $openvrPaths) {
        $paths = Get-Content -LiteralPath $openvrPaths -Raw | ConvertFrom-Json
        if ($paths.config -and $paths.config.Count -gt 0) {
            return (Join-Path ([string]$paths.config[0]) "steamvr.vrsettings")
        }
    }
    return "C:\Program Files (x86)\Steam\config\steamvr.vrsettings"
}

function Get-AnyaDanceBackupPath {
    return Join-Path (Join-Path $env:LOCALAPPDATA "AnyaDance") "steamvr.vrsettings.backup"
}

# The exact driver-root path registered, recorded in the stable per-user AppData
# folder. Shared with the in-UI register/unregister (same file name), so either
# entry point can clean up the other's registration even after the bundle moves.
function Get-AnyaDanceRegisteredPathRecord {
    return Join-Path (Join-Path $env:LOCALAPPDATA "AnyaDance") "registered_driver_path.txt"
}

# Write text as UTF-8 without a BOM so the C++ tool, which reads the record as raw
# bytes, matches the path exactly.
function Write-AnyaDanceTextFile {
    param([string]$Path, [string]$Content)
    [System.IO.File]::WriteAllText($Path, $Content, (New-Object System.Text.UTF8Encoding($false)))
}

function Resolve-SteamExe {
    param([string]$SteamExe)

    if ($SteamExe -and (Test-Path $SteamExe)) { return (Resolve-Path $SteamExe).Path }
    foreach ($SteamRoot in Get-SteamInstallRoots) {
        $Candidate = Join-Path $SteamRoot "steam.exe"
        if (Test-Path $Candidate) { return (Resolve-Path $Candidate).Path }
    }
    $Default = "C:\Program Files (x86)\Steam\steam.exe"
    if (Test-Path $Default) { return (Resolve-Path $Default).Path }
    throw "Could not find Steam.exe. Pass -SteamExe with the correct path."
}