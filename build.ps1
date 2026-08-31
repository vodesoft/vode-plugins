#Requires -Version 5.1
<#
.SYNOPSIS
    Vode Plugins (vdplg) - build everything.

.DESCRIPTION
    Configures (if needed) and builds ALL targets of the repo in the given
    configuration (default: Release) into the ./build directory:
      * VST3 plugin bundles   -> build/VST3/<Config>/*.vst3
      * common library, testhost, unit tests, moduleinfotool, testdata

    Uses the already-configured ./build directory when present; otherwise
    runs cmake configure first. CMake is resolved from PATH, falling back
    to the Visual Studio bundled copy (see setup-dev-env.ps1).

.PARAMETER Configuration
    Build configuration. Default: Release.

.PARAMETER Clean
    Reconfigure from scratch (deletes the build directory first).

.EXAMPLE
    .\build.ps1
.EXAMPLE
    .\build.ps1 -Configuration Debug
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $repoRoot 'build'

# --- locate cmake -----------------------------------------------------------
$cmakeExe = $null
$cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCmd) {
    $cmakeExe = $cmakeCmd.Source
} else {
    # vswhere lookup for VS-bundled cmake
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsInstall = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null | Select-Object -First 1
        if ($vsInstall) {
            $candidate = Join-Path $vsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path $candidate) { $cmakeExe = $candidate }
        }
    }
}
if (-not $cmakeExe) {
    throw "cmake not found (PATH or VS-bundled). Run .\setup-dev-env.ps1 to check prerequisites."
}
Write-Host "[vdplg] cmake: $cmakeExe"

# --- clean / configure ------------------------------------------------------
if ($Clean) {
    Write-Host "[vdplg] -Clean specified, removing $buildDir"
    if (Test-Path $buildDir) { Remove-Item $buildDir -Recurse -Force }
}

$cacheFile = Join-Path $buildDir 'CMakeCache.txt'
if (-not (Test-Path $cacheFile)) {
    Write-Host "[vdplg] Configuring (one-time)..."
    & $cmakeExe -S $repoRoot -B $buildDir -G "Visual Studio 17 2022" -A x64
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed (exit $LASTEXITCODE)" }
}

# --- build all targets --------------------------------------------------------
Write-Host "[vdplg] Building ALL targets (config: $Configuration)..."
& $cmakeExe --build $buildDir --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)" }

# --- summary -------------------------------------------------------------------
$vst3Dir = Join-Path $buildDir "VST3\$Configuration"
$bundles = @()
if (Test-Path $vst3Dir) {
    $bundles = Get-ChildItem -Path $vst3Dir -Filter *.vst3 -Directory
}
Write-Host ""
Write-Host "[vdplg] Build succeeded. VST3 bundles:"
foreach ($b in $bundles) { Write-Host "  - $($b.FullName)" }
if ($bundles.Count -eq 0) {
    Write-Warning "No .vst3 bundles found under $vst3Dir"
}
