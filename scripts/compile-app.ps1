# compile-app.ps1 — Build the Android APK
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Write-Host "Building Android app..." -ForegroundColor Cyan
& "$root\gradlew.bat" assembleDebug
if ($LASTEXITCODE -ne 0) { throw "app build failed" }
Write-Host "APK built: cardboardplusplus-android\build\outputs\apk\debug\app-debug.apk" -ForegroundColor Green
