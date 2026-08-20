# install-app.ps1 — Install the APK to a connected Android phone via ADB
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$apk = "$root\cardboardplusplus-android\build\outputs\apk\debug\app-debug.apk"

# Find adb: prefer ANDROID_HOME/ANDROID_SDK_ROOT, fall back to default path
$adb = $null
foreach ($envVar in @("ANDROID_HOME", "ANDROID_SDK_ROOT")) {
    $val = [System.Environment]::GetEnvironmentVariable($envVar, "User")
    if ($val -and (Test-Path "$val\platform-tools\adb.exe")) { $adb = "$val\platform-tools\adb.exe"; break }
}
if (-not $adb) {
    $default = "$root\..\AppData\Local\Android\Sdk\platform-tools\adb.exe"
    if (Test-Path $default) { $adb = $default }
}
if (-not $adb) { throw "adb.exe not found — set ANDROID_HOME or install Android SDK" }

if (-not (Test-Path $apk)) {
    throw "APK not found at $apk — run scripts\compile-app.ps1 first"
}

Write-Host "Checking for connected devices..." -ForegroundColor Cyan
$devices = & $adb devices 2>&1
if ($devices -notmatch "device$") { throw "No Android device connected — plug in a phone and enable USB debugging" }

Write-Host "Installing APK to phone..." -ForegroundColor Cyan
& $adb install -r $apk
if ($LASTEXITCODE -ne 0) { throw "adb install failed" }
Write-Host "APK installed successfully" -ForegroundColor Green
