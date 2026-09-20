# Builds the SteamVR driver DLL (Release|x64 by default): ensures the FFmpeg
# runtime, bakes the version header, locates MSBuild via vswhere, and builds
# the solution. Fails the script when MSBuild is missing or the build fails.
param([string]$Configuration = "Release")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$sln = "$root\driver_cardboardplusplus\driver_cardboardplusplus.sln"
. "$PSScriptRoot\ffmpeg-deps.ps1"
Ensure-FfmpegDeps "$root\driver_cardboardplusplus\lib\ffmpeg"
& "$PSScriptRoot\bake-driver-version.ps1"
$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vsWhere)) { throw "vswhere.exe not found - install Visual Studio" }
$msbuild = & $vsWhere -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\amd64\MSBuild.exe" | Select-Object -First 1
if (-not $msbuild) { throw "MSBuild not found - install the C++ build tools in Visual Studio" }
Write-Host "Building driver ($Configuration|x64)..." -ForegroundColor Cyan
& $msbuild $sln /p:Configuration=$Configuration /p:Platform=x64 /v:minimal
if ($LASTEXITCODE -ne 0) { throw "driver build failed" }
Write-Host "Driver built: driver_cardboardplusplus\x64\$Configuration\driver_cardboardplusplus.dll" -ForegroundColor Green
