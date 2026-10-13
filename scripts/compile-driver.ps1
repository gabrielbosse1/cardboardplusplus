# Builds the SteamVR driver DLL (Release|x64 by default): ensures the FFmpeg
# runtime, bakes the version header, locates MSBuild via vswhere, builds the
# solution, then compiles and runs the driver unit tests. Fails the script when
# MSBuild is missing or the build fails.
param([string]$Configuration = "Release")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$sln = "$root\driver_cardboardplusplus\driver_cardboardplusplus.sln"
. "$PSScriptRoot\ffmpeg-deps.ps1"
Ensure-FfmpegDeps "$root\driver_cardboardplusplus\lib\ffmpeg"
& "$PSScriptRoot\bake-driver-version.ps1"
$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vsWhere)) { throw "vswhere.exe not found - install Visual Studio" }
# Native stderr must not be fatal under Stop (external tools always write
# there); the exit code after each call is the real failure signal.
$nativePref = $ErrorActionPreference
$ErrorActionPreference = "Continue"
$LASTEXITCODE = 0
$msbuild = & $vsWhere -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\amd64\MSBuild.exe" | Select-Object -First 1
$vsWhereExit = $LASTEXITCODE
$ErrorActionPreference = $nativePref
if ($vsWhereExit -ne 0) { throw "vswhere failed" }
if (-not $msbuild) { throw "MSBuild not found - install the C++ build tools in Visual Studio" }
Write-Host "Building driver ($Configuration|x64)..." -ForegroundColor Cyan
$ErrorActionPreference = "Continue"
$LASTEXITCODE = 0
& $msbuild $sln /p:Configuration=$Configuration /p:Platform=x64 /v:minimal
$nativeExit = $LASTEXITCODE
$ErrorActionPreference = $nativePref
if ($nativeExit -ne 0) { throw "driver build failed" }
Write-Host "Driver built: driver_cardboardplusplus\x64\$Configuration\driver_cardboardplusplus.dll" -ForegroundColor Green

Write-Host "Running driver tests..." -ForegroundColor Cyan
$clPath = & $vsWhere -latest -find "VC\Tools\MSVC\**\bin\Hostx64\x64\cl.exe" | Select-Object -First 1
if (-not $clPath) { throw "cl.exe not found - cannot build driver tests" }
$vsRoot = & $vsWhere -latest -property installationPath
$devCmd = Join-Path $vsRoot "Common7\Tools\VsDevCmd.bat"
if (-not (Test-Path $devCmd)) { throw "VsDevCmd.bat not found - cannot build driver tests" }
$testObj = Join-Path $env:TEMP "cbpp_driver_tests"
New-Item -ItemType Directory -Force -Path $testObj | Out-Null
$testExe = Join-Path $testObj "driver_tests.exe"
# Delete the previous binary first: cl exits non-zero on failure, but if the error is
# swallowed anywhere below, running a stale exe would report a green suite that no
# longer matches the source.
if (Test-Path $testExe) { Remove-Item -Force $testExe }
$testCmd = "`"$devCmd`" -arch=x64 >nul && cd /d `"$testObj`" && cl /nologo /EHsc /std:c++14 /I `"$root\driver_cardboardplusplus\include`" /Fe:driver_tests.exe `"$root\driver_cardboardplusplus\tests\test_wire_protocol.cpp`""
$ErrorActionPreference = "Continue"
$LASTEXITCODE = 0
$testBuildLog = cmd /c $testCmd 2>&1
$testCompileExit = $LASTEXITCODE
$ErrorActionPreference = $nativePref
$testBuildLog | ForEach-Object { Write-Host $_ }
if ($testCompileExit -ne 0 -or -not (Test-Path $testExe)) {
    throw "driver tests failed to build (cl exit $testCompileExit)"
}
& $testExe
if ($LASTEXITCODE -ne 0) { throw "driver tests failed" }
Write-Host "Driver tests passed" -ForegroundColor Green
