# Builds the debug APK via the Gradle wrapper.
# Produces cardboardplusplus-android\build\outputs\apk\debug\app-debug.apk
# for install-app.ps1. Fails the script when Gradle fails.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Write-Host "Building Android app..." -ForegroundColor Cyan
& "$root\gradlew.bat" assembleDebug
if ($LASTEXITCODE -ne 0) { throw "app build failed" }
Write-Host "APK built: cardboardplusplus-android\build\outputs\apk\debug\app-debug.apk" -ForegroundColor Green
