param(
    [string]$Configuration = "Release",
    [string]$Arch = "x64",
    [string]$Generator,
    [string]$OpenVRSdkRoot,
    [string]$ImguiRoot
)
$ErrorActionPreference = "Stop"

Write-Host "Building AnyaDance ($Configuration, $Arch)" -ForegroundColor Cyan

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildDir = Join-Path $RepoRoot "build"

function Get-CMakeGeneratorNames {
    $Help = cmake --help
    $Generators = @()
    foreach ($Line in $Help) {
        if ($Line -match "^\s*\*?\s*([A-Za-z0-9][^=]+?)\s+=") { $Generators += $Matches[1].Trim() }
    }
    return $Generators
}

function Get-VisualStudioGenerator {
    param([string[]]$AvailableGenerators)
    $VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $VsWhere)) { return $null }
    $InstallationVersion = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
    if (-not $InstallationVersion) { return $null }
    $MajorVersion = ($InstallationVersion -split "\.")[0]
    $GeneratorByMajorVersion = @{
        "18" = "Visual Studio 18 2026"
        "17" = "Visual Studio 17 2022"
        "16" = "Visual Studio 16 2019"
    }
    $VisualStudioGenerator = $GeneratorByMajorVersion[$MajorVersion]
    if ($VisualStudioGenerator -and $AvailableGenerators -contains $VisualStudioGenerator) { return $VisualStudioGenerator }
    return $null
}

function Test-CommandAvailable { param([string]$Name) return $null -ne (Get-Command $Name -ErrorAction SilentlyContinue) }

function Select-DefaultGenerator {
    param([string[]]$AvailableGenerators)
    $VisualStudioGenerator = Get-VisualStudioGenerator -AvailableGenerators $AvailableGenerators
    if ($VisualStudioGenerator) { return $VisualStudioGenerator }
    $HasCompiler = (Test-CommandAvailable "cl") -or (Test-CommandAvailable "clang-cl")
    if ($HasCompiler -and (Test-CommandAvailable "ninja") -and ($AvailableGenerators -contains "Ninja Multi-Config")) { return "Ninja Multi-Config" }
    if ($HasCompiler -and (Test-CommandAvailable "ninja") -and ($AvailableGenerators -contains "Ninja")) { return "Ninja" }
    if ($HasCompiler -and (Test-CommandAvailable "nmake") -and ($AvailableGenerators -contains "NMake Makefiles")) { return "NMake Makefiles" }
    return $null
}

function Invoke-Checked {
    param([string]$FilePath, [string[]]$Arguments)
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$FilePath failed with exit code $LASTEXITCODE" }
}

function Get-CMakeCacheValue {
    param([string]$CachePath, [string]$Name)
    if (-not (Test-Path $CachePath)) { return $null }
    $EscapedName = [regex]::Escape($Name)
    $Match = Select-String -Path $CachePath -Pattern "^${EscapedName}:[^=]*=(.*)$" | Select-Object -First 1
    if (-not $Match) { return $null }
    return $Match.Matches[0].Groups[1].Value
}

function Test-CMakeCacheCompatible {
    param(
        [string]$CachePath,
        [string]$ExpectedGenerator,
        [bool]$ExpectedGeneratorSupportsPlatform,
        [string]$ExpectedPlatform,
        [string]$ExpectedSourceDir
    )
    $CachedGenerator = Get-CMakeCacheValue -CachePath $CachePath -Name "CMAKE_GENERATOR"
    $CachedPlatform = Get-CMakeCacheValue -CachePath $CachePath -Name "CMAKE_GENERATOR_PLATFORM"
    $CachedSourceDir = Get-CMakeCacheValue -CachePath $CachePath -Name "CMAKE_HOME_DIRECTORY"
    if (-not $CachedGenerator) { return $true }
    $SourceDirMismatch = $false
    if ($ExpectedSourceDir -and $CachedSourceDir) {
        $SourceDirMismatch = [System.IO.Path]::GetFullPath($CachedSourceDir) -ne [System.IO.Path]::GetFullPath($ExpectedSourceDir)
    }
    return -not (
        $SourceDirMismatch -or
        ($ExpectedGenerator -and $CachedGenerator -ne $ExpectedGenerator) -or
        ($ExpectedGeneratorSupportsPlatform -and $CachedPlatform -ne $ExpectedPlatform) -or
        (-not $ExpectedGeneratorSupportsPlatform -and $CachedPlatform)
    )
}

function Remove-CMakeCache {
    param([string]$Directory)
    $CachePath = Join-Path $Directory "CMakeCache.txt"
    if (Test-Path $CachePath) { Remove-Item -LiteralPath $CachePath -Force }
    $CMakeFilesDir = Join-Path $Directory "CMakeFiles"
    if (Test-Path $CMakeFilesDir) { Remove-Item -LiteralPath $CMakeFilesDir -Recurse -Force }
}

function Remove-IncompatibleCMakeCache {
    param(
        [string]$Directory,
        [string]$Description,
        [string]$ExpectedGenerator,
        [bool]$ExpectedGeneratorSupportsPlatform,
        [string]$ExpectedPlatform,
        [string]$ExpectedSourceDir
    )
    $CachePath = Join-Path $Directory "CMakeCache.txt"
    if (-not (Test-Path $CachePath)) { return }
    if (Test-CMakeCacheCompatible -CachePath $CachePath -ExpectedGenerator $ExpectedGenerator -ExpectedGeneratorSupportsPlatform $ExpectedGeneratorSupportsPlatform -ExpectedPlatform $ExpectedPlatform -ExpectedSourceDir $ExpectedSourceDir) { return }

    $CachedGenerator = Get-CMakeCacheValue -CachePath $CachePath -Name "CMAKE_GENERATOR"
    $CachedPlatform = Get-CMakeCacheValue -CachePath $CachePath -Name "CMAKE_GENERATOR_PLATFORM"
    $CachedSourceDir = Get-CMakeCacheValue -CachePath $CachePath -Name "CMAKE_HOME_DIRECTORY"
    Write-Host "Removing incompatible $Description CMake cache for generator '$CachedGenerator' platform '$CachedPlatform' source '$CachedSourceDir'." -ForegroundColor Yellow
    Remove-CMakeCache -Directory $Directory
}

$AvailableGenerators = Get-CMakeGeneratorNames
if (-not $Generator) {
    $Generator = Select-DefaultGenerator -AvailableGenerators $AvailableGenerators
    if (-not $Generator) {
        throw "Could not find a usable Windows C++ build environment. Install Visual Studio Build Tools with Desktop development with C++, or run from a Developer PowerShell and pass -Generator."
    }
}

$GeneratorSupportsPlatform = $Generator -match "^Visual Studio "
$GeneratorIsMultiConfig = $GeneratorSupportsPlatform -or $Generator -eq "Ninja Multi-Config"

$ConfigureArgs = @("-S", ".", "-B", "build")
if ($Generator) { $ConfigureArgs += @("-G", $Generator) }
if ($GeneratorSupportsPlatform) {
    $ConfigureArgs += @("-A", $Arch)
} elseif (-not $GeneratorIsMultiConfig) {
    $ConfigureArgs += "-DCMAKE_BUILD_TYPE=$Configuration"
}
# A supplied dependency root means use it instead of fetching.
if ($OpenVRSdkRoot) { $ConfigureArgs += @("-DOPENVR_SDK_ROOT=$OpenVRSdkRoot", "-DANYADANCE_FETCH_OPENVR=OFF") }
if ($ImguiRoot) { $ConfigureArgs += @("-DIMGUI_ROOT=$ImguiRoot", "-DANYADANCE_FETCH_IMGUI=OFF") }

Push-Location $RepoRoot | Out-Null
try {
    Remove-IncompatibleCMakeCache -Directory $BuildDir -Description "top-level" -ExpectedGenerator $Generator -ExpectedGeneratorSupportsPlatform $GeneratorSupportsPlatform -ExpectedPlatform $Arch -ExpectedSourceDir $RepoRoot
    Remove-IncompatibleCMakeCache -Directory (Join-Path $BuildDir "openvr-subbuild") -Description "OpenVR subbuild" -ExpectedGenerator $Generator -ExpectedGeneratorSupportsPlatform $GeneratorSupportsPlatform -ExpectedPlatform $Arch -ExpectedSourceDir (Join-Path $BuildDir "openvr-subbuild")
    Remove-IncompatibleCMakeCache -Directory (Join-Path $BuildDir "imgui-subbuild") -Description "Dear ImGui subbuild" -ExpectedGenerator $Generator -ExpectedGeneratorSupportsPlatform $GeneratorSupportsPlatform -ExpectedPlatform $Arch -ExpectedSourceDir (Join-Path $BuildDir "imgui-subbuild")

    Invoke-Checked -FilePath "cmake" -Arguments $ConfigureArgs
    Invoke-Checked -FilePath "cmake" -Arguments @("--build", "build", "--config", $Configuration)
    $DriverRoot = Join-Path $RepoRoot "build\out\anyadance"
    Write-Host "Staged driver root: $DriverRoot" -ForegroundColor Green
    Write-Host "Tool executable: $(Join-Path $DriverRoot 'AnyaDance.exe')" -ForegroundColor Green
}
finally {
    Pop-Location | Out-Null
}
