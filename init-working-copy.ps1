#Requires -Version 5.1
<#
.SYNOPSIS
    Vode Plugins (vdplg) - idempotent working-copy initializer.

.DESCRIPTION
    Prepares a fresh or existing clone for building:
      1. Preflight: repo root detected, git available, submodules registered.
      2. `git submodule sync --recursive` then
         `git submodule update --init --recursive --force`.
         If the recursive update fails with a permission error on the nested
         vst3sdk submodules (known Windows quirk), retries the update from
         INSIDE third_party/vst3sdk.
      3. Sentinel verification: key files of each pinned dependency exist.
      4. Build dir prep: creates ./build and warns (without deleting) when a
         CMake cache exists whose source hash differs (.vdplg-cmake-hash).
      5. Optional -Configure: runs cmake configure with the VS 2022 x64
         generator (system cmake or VS-bundled fallback).

    The script is safe to re-run at any time.

.PARAMETER Configure
    Also run CMake configure after submodule setup.

.PARAMETER BuildDir
    Build directory (default: <repo>\build).

.EXAMPLE
    .\init-working-copy.ps1
    .\init-working-copy.ps1 -Configure
#>
[CmdletBinding()]
param(
    [switch]$Configure,
    [string]$BuildDir = ''
)

$ErrorActionPreference = 'Stop'

# --- locate repo root --------------------------------------------------------
$RepoRoot = $PSScriptRoot
if (-not (Test-Path (Join-Path $RepoRoot '.gitmodules'))) {
    throw "Could not find .gitmodules in $RepoRoot - run this script from the repo root."
}
Write-Host "Repo root: $RepoRoot" -ForegroundColor Cyan

Push-Location $RepoRoot
try {
    # --- preflight -----------------------------------------------------------
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        throw 'git not found on PATH. Install Git for Windows first.'
    }
    Write-Host ('git ' + (& git --version))

    # --- submodules ------------------------------------------------------------
    Write-Host ''
    Write-Host 'Syncing submodule URLs...' -ForegroundColor Cyan
    & git submodule sync --recursive
    if ($LASTEXITCODE -ne 0) { throw 'git submodule sync failed' }

    Write-Host 'Updating submodules (this may take a while on first run)...' -ForegroundColor Cyan
    & git submodule update --init --recursive --force
    if ($LASTEXITCODE -ne 0) {
        Write-Warning 'Recursive submodule update failed - retrying from inside third_party\vst3sdk (known Windows permission quirk).'
        $sdkDir = Join-Path $RepoRoot 'third_party\vst3sdk'
        if (-not (Test-Path $sdkDir)) {
            New-Item -ItemType Directory -Path $sdkDir -Force | Out-Null
        }
        Push-Location $sdkDir
        try {
            & git submodule update --init --recursive --force
            if ($LASTEXITCODE -ne 0) { throw 'nested vst3sdk submodule update failed' }
        } finally {
            Pop-Location
        }
    }

    # --- sentinel verification ---------------------------------------------------
    Write-Host ''
    Write-Host 'Verifying dependency sentinels...' -ForegroundColor Cyan
    $sentinels = @(
        @{ Name = 'vst3sdk';        Path = 'third_party\vst3sdk\pluginterfaces\vst\ivstcomponent.h' },
        @{ Name = 'signalsmith-dsp'; Path = 'third_party\signalsmith-dsp\filters.h' },
        @{ Name = 'dr_wav';         Path = 'third_party\dr_wav\dr_wav.h' },
        @{ Name = 'catch2';         Path = 'third_party\catch2\CMakeLists.txt' }
    )
    $allOk = $true
    foreach ($s in $sentinels) {
        $full = Join-Path $RepoRoot $s.Path
        if (Test-Path $full) {
            Write-Host ("[OK]   {0}" -f $s.Name) -ForegroundColor Green
        } else {
            Write-Host ("[MISS] {0} - expected {1}" -f $s.Name, $s.Path) -ForegroundColor Red
            $allOk = $false
        }
    }
    if (-not $allOk) {
        throw 'One or more dependencies are missing. Re-run: git submodule update --init --recursive --force'
    }

    # --- build dir prep ------------------------------------------------------------
    if ([string]::IsNullOrWhiteSpace($BuildDir)) { $BuildDir = Join-Path $RepoRoot 'build' }
    if (-not (Test-Path $BuildDir)) {
        New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
        Write-Host "Created build directory: $BuildDir" -ForegroundColor Cyan
    }

    $hashFile = Join-Path $BuildDir '.vdplg-cmake-hash'
    $srcHash = (Get-FileHash (Join-Path $RepoRoot 'CMakeLists.txt') -Algorithm SHA256).Hash
    if ((Test-Path (Join-Path $BuildDir 'CMakeCache.txt')) -and (Test-Path $hashFile)) {
        $oldHash = (Get-Content $hashFile -Raw).Trim()
        if ($oldHash -ne $srcHash) {
            Write-Warning 'Top-level CMakeLists.txt changed since the last configure; the CMake cache may be stale. Consider deleting the build directory.'
        }
    }
    Set-Content -Path $hashFile -Value $srcHash -NoNewline

    # --- optional configure -----------------------------------------------------------
    if ($Configure) {
        Write-Host ''
        Write-Host 'Configuring with CMake...' -ForegroundColor Cyan
        $cmakeExe = (Get-Command cmake -ErrorAction SilentlyContinue).Source
        if (-not $cmakeExe) {
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
        if (-not $cmakeExe) { throw 'cmake not found (PATH or VS-bundled).' }
        & $cmakeExe -B $BuildDir -G 'Visual Studio 17 2022' -A x64 $RepoRoot
        if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
        Write-Host 'Configure complete.' -ForegroundColor Green
    }

    Write-Host ''
    Write-Host 'Working copy ready.' -ForegroundColor Green
} finally {
    Pop-Location
}
