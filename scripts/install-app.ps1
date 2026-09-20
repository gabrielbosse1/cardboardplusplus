# Installs the debug APK onto the connected phone: resolves adb.exe (SDK env
# vars, then the default LOCALAPPDATA install), requires exactly one device,
# then `adb install -r -g`. Ask the user for the IP first for network ADB —
# never push to a network phone unconfirmed.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$apk = "$root\cardboardplusplus-android\build\outputs\apk\debug\app-debug.apk"
$adb = $null
foreach ($envVar in @("ANDROID_HOME", "ANDROID_SDK_ROOT")) {
    foreach ($scope in @("User", "Machine")) {
        $val = [System.Environment]::GetEnvironmentVariable($envVar, $scope)
        if ($val -and (Test-Path "$val\platform-tools\adb.exe")) { $adb = "$val\platform-tools\adb.exe"; break }
    }
    if ($adb) { break }
}
if (-not $adb) {
    $default = Join-Path $env:LOCALAPPDATA "Android\Sdk\platform-tools\adb.exe"
    if (Test-Path $default) { $adb = $default }
}
if (-not $adb) { throw "adb.exe not found - set ANDROID_HOME or install Android SDK" }
if (-not (Test-Path $apk)) {
    throw "APK not found at $apk - run scripts\compile-app.ps1 first"
}
Write-Host "Checking for connected devices..." -ForegroundColor Cyan
$devices = & $adb devices 2>&1
$attached = @($devices | Select-String "`tdevice$" | ForEach-Object { $_.Line })
if ($attached.Count -eq 0) { throw "No Android device connected - plug in a phone and enable USB debugging" }
if ($attached.Count -gt 1) {
    $serials = ($attached | ForEach-Object { ($_ -split "`t")[0] }) -join ", "
    throw "Multiple devices connected ($serials) - re-run with a target, e.g.: & `$adb -s <serial> install -r -g $apk"
}
Write-Host "Installing APK to phone..." -ForegroundColor Cyan
& $adb install -r -g $apk
if ($LASTEXITCODE -ne 0) { throw "adb install failed" }
Write-Host "APK installed successfully" -ForegroundColor Green
