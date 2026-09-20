# Full build gate: bridge, driver, app, then the installer (reusing the
# just-built bridge via -SkipBridge). Run after touching shared contracts.
# -Release flips bridge/driver/installer to release configs.
param([switch]$Release)
$ErrorActionPreference = "Stop"
Write-Host "=== Building all components ===" -ForegroundColor Yellow
& "$PSScriptRoot\compile-bridge.ps1" -Release:$Release
& "$PSScriptRoot\compile-driver.ps1" -Configuration $(if ($Release) { "Release" } else { "Debug" })
& "$PSScriptRoot\compile-app.ps1"
& "$PSScriptRoot\compile-installer.ps1" -Release:$Release -SkipBridge
Write-Host "`n=== All builds succeeded ===" -ForegroundColor Green
