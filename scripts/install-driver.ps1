# install-driver.ps1 — Install the SteamVR driver to the SteamVR drivers directory
param([string]$SteamVR = "C:\Program Files (x86)\Steam\steamapps\common\SteamVR")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$srcDll = "$root\driver_cardboardplusplus\x64\Release\driver_cardboardplusplus.dll"
$srcFfmpeg = "$root\driver_cardboardplusplus\lib\ffmpeg\bin"
$dstDir = "$SteamVR\drivers\cardboardplusplus\bin\win64"

if (-not (Test-Path $srcDll)) {
    throw "Driver DLL not found at $srcDll — run scripts\compile-driver.ps1 first"
}

if (-not (Test-Path "$SteamVR\drivers")) {
    throw "SteamVR drivers directory not found at $SteamVR\drivers — check -SteamVR path or install SteamVR"
}

Write-Host "Installing driver to $dstDir ..." -ForegroundColor Cyan
New-Item -ItemType Directory -Path $dstDir -Force | Out-Null
Copy-Item $srcDll $dstDir -Force
if (Test-Path $srcFfmpeg) {
    Copy-Item "$srcFfmpeg\*.dll" $dstDir -Force
}

# Copy the driver manifest
$manifestSrc = "$root\driver_cardboardplusplus\resources\driver.vrdrivermanifest"
$manifestDst = "$SteamVR\drivers\cardboardplusplus"
if (Test-Path $manifestSrc) {
    Copy-Item $manifestSrc $manifestDst -Force
}

Write-Host "Driver installed to $dstDir" -ForegroundColor Green
Write-Host "Restart SteamVR to load the driver." -ForegroundColor Yellow
