# build-windows.ps1 — release-grade Windows .exe build, full self-bootstrap.
#
# Host requirement: **Windows Server 2016+ / Windows 10+ with admin PowerShell**.
# No MSYS2 / no toolchain pre-install. This script:
#   1. Calls build-prep.ps1 (installs MSYS2 + pacman toolchain if missing).
#   2. Runs vendor-fetch + vendor-build + release inside the mingw64 environment.
#   3. Verifies dist artifacts.
#
# Usage (from repo root, admin PowerShell):
#   .\windows-agent\scripts\build-windows.ps1
#
# Output:
#   windows-agent\dist\assessment-agent.exe
#   windows-agent\dist\SHA256SUMS
#
# Re-runnable (idempotent). First run installs MSYS2 (~10 minutes including
# package downloads); subsequent runs just rebuild (~2-5 min).

#Requires -RunAsAdministrator

[CmdletBinding()]
param(
    [string]$Msys2Root = 'C:\msys64',
    [switch]$SkipPrep   # 이미 prep 끝났으면 생략
)

$ErrorActionPreference = 'Stop'

$scriptRoot = $PSScriptRoot
$winAgentRoot = (Resolve-Path "$scriptRoot\..").Path

Write-Host ''
Write-Host '=== Windows agent release build ==='
Write-Host "  repo: $winAgentRoot"
Write-Host "  MSYS2: $Msys2Root"
Write-Host ''

# --- 1. prep (idempotent: MSYS2 + pacman toolchain) ----------------------------
if (-not $SkipPrep) {
    & "$scriptRoot\build-prep.ps1" -Msys2Root $Msys2Root
    if ($LASTEXITCODE -ne 0) { throw "build-prep.ps1 failed" }
}

$bashExe = Join-Path $Msys2Root 'usr\bin\bash.exe'
if (-not (Test-Path $bashExe)) {
    throw "$bashExe missing — run without -SkipPrep first"
}

# --- 2. Convert Windows path to MSYS path ---------------------------------------
# C:\Users\foo\repo → /c/Users/foo/repo
$drive = $winAgentRoot.Substring(0, 1).ToLower()
$rest  = $winAgentRoot.Substring(2) -replace '\\', '/'
$msysPath = "/$drive$rest"

# --- 3. Build inside mingw64 ----------------------------------------------------
Write-Host "[build-windows] vendor-fetch + vendor-build + release..."

$env:MSYSTEM = 'MINGW64'
$env:CHERE_INVOKING = '1'

$buildCmd = @"
set -e
cd '$msysPath'
export PATH=/mingw64/bin:`$PATH
make vendor-fetch
make vendor-build
make release
"@

& $bashExe -lc $buildCmd
if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }

# --- 4. Verify artifacts --------------------------------------------------------
$dist = Join-Path $winAgentRoot 'dist'
$exe  = Join-Path $dist 'assessment-agent.exe'
$shas = Join-Path $dist 'SHA256SUMS'

if (-not (Test-Path $exe)) { throw "expected $exe not produced" }
if (-not (Test-Path $shas)) { throw "expected $shas not produced" }

Write-Host ''
Write-Host '[build-windows] OK — release artifacts:'
Get-ChildItem $dist | Format-Table Name, Length, LastWriteTime -AutoSize
Get-Content $shas
