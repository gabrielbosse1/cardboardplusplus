# compile-all.ps1 — Build all components
param([switch]$Release)
$ErrorActionPreference = "Stop"
Write-Host "=== Building all components ===" -ForegroundColor Yellow

& "$PSScriptRoot\compile-bridge.ps1" -Release:$Release
& "$PSScriptRoot\compile-driver.ps1" -Configuration $(if ($Release) { "Release" } else { "Debug" })
& "$PSScriptRoot\compile-app.ps1"

Write-Host "`n=== All builds succeeded ===" -ForegroundColor Green
