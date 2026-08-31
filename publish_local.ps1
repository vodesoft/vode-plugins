#Requires -Version 5.1
<#
.SYNOPSIS
    Vode Plugins (vdplg) - build and publish VST3 bundles locally.

.DESCRIPTION
    Two-phase publish flow:
      Phase 1 (regular user): calls .\build.ps1 to build ALL targets.
      Phase 2 (elevated):     re-launches itself with administrator rights
                              and copies every built *.vst3 bundle into
                              C:\Program Files\Common Files\VST3\Vode Plugins\
                              (replacing any previously published version).

    The elevated phase is triggered automatically via a UAC prompt
    (Start-Process -Verb RunAs). You do NOT need to start this script
    as administrator yourself.

.PARAMETER Configuration
    Build configuration passed through to build.ps1. Default: Release.

.PARAMETER CopyOnly
    Internal: skip the build and only perform the elevated copy phase.
    Used by the self-relaunch; not intended for direct use.

.EXAMPLE
    .\publish_local.ps1
.EXAMPLE
    .\publish_local.ps1 -Configuration Debug
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [switch]$CopyOnly
)

$ErrorActionPreference = 'Stop'

$repoRoot   = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir   = Join-Path $repoRoot 'build'
$vst3Dir    = Join-Path $buildDir "VST3\$Configuration"
$publishDir = 'C:\Program Files\Common Files\VST3\Vode Plugins'

function Get-BuiltBundles {
    if (-not (Test-Path $script:vst3Dir)) { return @() }
    return @(Get-ChildItem -Path $script:vst3Dir -Filter *.vst3 -Directory)
}

# --- Phase 1: build as regular user ------------------------------------------
if (-not $CopyOnly) {
    Write-Host "[vdplg] Phase 1/2: building (config: $Configuration) as regular user..."
    & (Join-Path $repoRoot 'build.ps1') -Configuration $Configuration
    if ($LASTEXITCODE -ne 0) { throw "build.ps1 failed (exit $LASTEXITCODE)" }

    $bundles = @(Get-BuiltBundles)
    if ($bundles.Count -eq 0) {
        throw "No .vst3 bundles found under $vst3Dir - nothing to publish."
    }
    Write-Host "[vdplg] Found $($bundles.Count) bundle(s):"
    foreach ($b in $bundles) { Write-Host "  - $($b.Name)" }

    # --- Phase 2: relaunch elevated; the child re-scans the same build dir ---
    Write-Host ""
    Write-Host "[vdplg] Phase 2/2: requesting elevation to copy into $publishDir ..."
    $scriptPath = $MyInvocation.MyCommand.Path
    $argLine = "-NoProfile -ExecutionPolicy Bypass -File `"$scriptPath`" -CopyOnly -Configuration $Configuration"
    $proc = Start-Process -FilePath 'powershell.exe' -ArgumentList $argLine -Verb RunAs -Wait -PassThru
    if ($proc.ExitCode -ne 0) {
        throw "Elevated copy phase failed (exit $($proc.ExitCode)). If you cancelled the UAC prompt, re-run this script."
    }
    Write-Host "[vdplg] Publish complete."
    return
}

# --- Phase 2 (elevated): copy bundles -----------------------------------------
$bundles = @(Get-BuiltBundles)

if ($bundles.Count -eq 0) {
    throw "No .vst3 bundles to copy."
}

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    throw "Copy phase must run elevated. Re-run .\publish_local.ps1 (it will request elevation itself)."
}

New-Item -ItemType Directory -Path $publishDir -Force | Out-Null

foreach ($b in $bundles) {
    $src = $b.FullName
    $dst = Join-Path $publishDir $b.Name
    Write-Host "[vdplg] Copying $src -> $dst"
    if (Test-Path $dst) {
        Remove-Item $dst -Recurse -Force
    }
    Copy-Item -Path $src -Destination $dst -Recurse -Force
}

Write-Host ""
Write-Host "[vdplg] Published $($bundles.Count) bundle(s) to $publishDir"
