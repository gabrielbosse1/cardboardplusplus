# Builds the bridge workspace (debug by default, -Release for release).
# Produces target\<cfg>\cardboard-bridge-svc.exe, the desktop hub binary.
# Fails the script when cargo fails (LASTEXITCODE check).
param([switch]$Release)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$cfg = if ($Release) { "release" } else { "debug" }
Write-Host "Building bridge ($cfg)..." -ForegroundColor Cyan
cargo build $(if ($Release) { "--release" }) --manifest-path "$root\bridge\Cargo.toml"
if ($LASTEXITCODE -ne 0) { throw "bridge build failed" }
Write-Host "Bridge built: target\$cfg\cardboard-bridge-svc.exe" -ForegroundColor Green
