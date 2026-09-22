# Builds the debug APK via the Gradle wrapper.
# Produces cardboardplusplus-android\build\outputs\apk\debug\app-debug.apk
# for install-app.ps1. Fails the script when Gradle fails.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Write-Host "Building Android app..." -ForegroundColor Cyan
# Native stderr must not be fatal under Stop (Gradle always writes there);
# the exit code below is the real failure signal.
$nativePref = $ErrorActionPreference
$ErrorActionPreference = "Continue"
& "$root\gradlew.bat" assembleDebug
$nativeExit = $LASTEXITCODE
$ErrorActionPreference = $nativePref
if ($nativeExit -ne 0) { throw "app build failed" }
Write-Host "APK built: cardboardplusplus-android\build\outputs\apk\debug\app-debug.apk" -ForegroundColor Green
