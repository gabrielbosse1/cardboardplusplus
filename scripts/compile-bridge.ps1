# compile-bridge.ps1 — Build the Rust bridge
param([switch]$Release)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$cfg = if ($Release) { "release" } else { "debug" }
Write-Host "Building bridge ($cfg)..." -ForegroundColor Cyan
cargo build $(if ($Release) { "--release" }) --manifest-path "$root\bridge\Cargo.toml"
if ($LASTEXITCODE -ne 0) { throw "bridge build failed" }
Write-Host "Bridge built: target\$cfg\cardboard-bridge.exe" -ForegroundColor Green
