# compile-driver.ps1 — Build the SteamVR driver DLL
param([string]$Configuration = "Release")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$sln = "$root\driver_cardboardplusplus\driver_cardboardplusplus.sln"

$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vsWhere)) { throw "vswhere.exe not found — install Visual Studio" }
$msbuild = & $vsWhere -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\amd64\MSBuild.exe" | Select-Object -First 1
if (-not $msbuild) { throw "MSBuild not found — install the C++ build tools in Visual Studio" }

Write-Host "Building driver ($Configuration|x64)..." -ForegroundColor Cyan
& $msbuild $sln /p:Configuration=$Configuration /p:Platform=x64 /v:minimal
if ($LASTEXITCODE -ne 0) { throw "driver build failed" }
Write-Host "Driver built: driver_cardboardplusplus\x64\$Configuration\driver_cardboardplusplus.dll" -ForegroundColor Green
