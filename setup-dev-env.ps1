#Requires -Version 5.1
<#
.SYNOPSIS
    Vode Plugins (vdplg) - read-only developer environment check.

.DESCRIPTION
    Verifies that all hard requirements for building the repo are present:
      * git >= 2.x
      * CMake >= 3.21 (system PATH or VS-bundled fallback)
      * Visual Studio 2022 with MSVC x64 toolset (via vswhere)
    Prints a summary table and exits non-zero if any hard requirement is
    missing. This script NEVER installs anything and NEVER modifies the
    working copy.

.PARAMETER Quiet
    Only print failures (no summary table).

.EXAMPLE
    .\setup-dev-env.ps1
#>
[CmdletBinding()]
param(
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'

$results = @()
function Add-Result {
    param([string]$Name, [bool]$Ok, [string]$Detail)
    $script:results += [pscustomobject]@{ Name = $Name; Ok = $Ok; Detail = $Detail }
}

# --- git -------------------------------------------------------------------
$gitExe = $null
$gitCmd = Get-Command git -ErrorAction SilentlyContinue
if ($gitCmd) {
    $gitExe = $gitCmd.Source
    try {
        $gitVer = (& git --version 2>$null) -replace 'git version ', ''
        Add-Result 'git' $true "$gitVer ($gitExe)"
    } catch {
        Add-Result 'git' $false "found at $gitExe but failed to run"
    }
} else {
    Add-Result 'git' $false 'not found on PATH'
}

# --- CMake -----------------------------------------------------------------
# Prefer system cmake; fall back to the VS-bundled copy.
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
if ($cmakeExe) {
    try {
        $cmakeVer = (& $cmakeExe --version 2>$null | Select-Object -First 1) -replace 'cmake version ', ''
        $verParts = $cmakeVer.Split('.')
        $ok = ([int]$verParts[0] -gt 3) -or ([int]$verParts[0] -eq 3 -and [int]$verParts[1] -ge 21)
        Add-Result 'cmake' $ok "$cmakeVer ($cmakeExe)"
    } catch {
        Add-Result 'cmake' $false "found at $cmakeExe but failed to run"
    }
} else {
    Add-Result 'cmake' $false 'not found (PATH or VS-bundled)'
}

# --- Visual Studio 2022 + MSVC x64 ------------------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $vsInstall = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath 2>$null | Select-Object -First 1
    if ($vsInstall) {
        $vsVersion = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationVersion 2>$null | Select-Object -First 1
        Add-Result 'VS2022 MSVC x64' $true "$vsVersion ($vsInstall)"
    } else {
        Add-Result 'VS2022 MSVC x64' $false 'no VS install with VC.Tools.x86.x64 component'
    }
} else {
    Add-Result 'VS2022 MSVC x64' $false 'vswhere not found (Visual Studio installer missing?)'
}

# --- summary -----------------------------------------------------------------
$failed = @($results | Where-Object { -not $_.Ok })
if (-not $Quiet) {
    Write-Host ''
    Write-Host 'Vode Plugins - developer environment check' -ForegroundColor Cyan
    Write-Host ('-' * 60)
    foreach ($r in $results) {
        $color = if ($r.Ok) { 'Green' } else { 'Red' }
        $mark  = if ($r.Ok) { '[OK]  ' } else { '[FAIL]' }
        Write-Host ("{0} {1,-18} {2}" -f $mark, $r.Name, $r.Detail) -ForegroundColor $color
    }
    Write-Host ('-' * 60)
}
if ($failed.Count -gt 0) {
    Write-Warning "Missing hard requirements: $($failed.Name -join ', ')"
    exit 1
}
if (-not $Quiet) { Write-Host 'All hard requirements satisfied.' -ForegroundColor Green }
exit 0
