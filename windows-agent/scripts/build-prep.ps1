# build-prep.ps1 — MSYS2 + mingw64 toolchain prerequisites installer.
#
# Server 2016+ / Windows 10+. Requires Administrator PowerShell.
#
# Two-step idempotent prep:
#   1. MSYS2 — if not at $Msys2Root, download silent installer from the
#      official msys2-installer GitHub release and install unattended.
#   2. Toolchain — pacman -S the packages vendor-build needs
#      (git, make, cmake, mingw-w64-x86_64-gcc, mingw-w64-x86_64-pkgconf, perl).
#      Re-running is no-op (--needed).
#
# Both steps are encapsulated by scripts/build-windows.ps1 — operators
# should not need to call this directly, but it is safe to.

#Requires -RunAsAdministrator

[CmdletBinding()]
param(
    [string]$Msys2Root = 'C:\msys64',
    [string]$Msys2Version = '20241208'
)

$ErrorActionPreference = 'Stop'

# Windows Server 2016 / older Windows 10 default to TLS 1.0 / 1.1. GitHub
# release downloads require TLS 1.2 — enable explicitly.
[Net.ServicePointManager]::SecurityProtocol =
    [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$bashExe = Join-Path $Msys2Root 'usr\bin\bash.exe'

# --- 1. MSYS2 silent install ---------------------------------------------------
if (-not (Test-Path $bashExe)) {
    Write-Host "[build-prep] MSYS2 not found — installing to $Msys2Root..."
    $url = "https://github.com/msys2/msys2-installer/releases/download/$Msys2Version/msys2-x86_64-$Msys2Version.exe"
    $installer = Join-Path $env:TEMP "msys2-installer-$Msys2Version.exe"

    Write-Host "[build-prep] downloading: $url"
    Invoke-WebRequest -Uri $url -OutFile $installer -UseBasicParsing

    Write-Host "[build-prep] running silent installer (this takes 1–2 min)..."
    # `in --confirm-command --accept-messages --root <dir>` is the documented
    # MSYS2 qt-installer-framework silent-install incantation.
    & $installer in --confirm-command --accept-messages --root $Msys2Root | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "MSYS2 installer exited with code $LASTEXITCODE"
    }
    Remove-Item $installer -Force -ErrorAction SilentlyContinue
}
if (-not (Test-Path $bashExe)) {
    throw "MSYS2 installation failed — $bashExe still missing"
}
Write-Host "[build-prep] MSYS2 ready at $Msys2Root"

# --- 2. pacman toolchain (idempotent via --needed) ------------------------------
Write-Host "[build-prep] syncing pacman + installing build toolchain..."

# First-run pacman key-init is bundled into MSYS2's post-install scripts which
# the silent installer runs automatically. A bare `pacman -Sy` here is enough.
& $bashExe -lc 'pacman -Sy --noconfirm --noprogressbar >/dev/null'
if ($LASTEXITCODE -ne 0) { throw "pacman -Sy failed (exit $LASTEXITCODE)" }

$pkgs = @(
    'git', 'make', 'cmake',
    'mingw-w64-x86_64-gcc',
    'mingw-w64-x86_64-pkgconf',
    'perl'
) -join ' '

& $bashExe -lc "pacman -S --noconfirm --needed --noprogressbar $pkgs"
if ($LASTEXITCODE -ne 0) { throw "pacman -S failed (exit $LASTEXITCODE)" }

Write-Host "[build-prep] OK — MSYS2 + mingw64 toolchain ready"
