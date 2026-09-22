# Builds the bridge workspace (debug by default, -Release for release).
# Produces target\<cfg>\cardboard-bridge-svc.exe, the desktop hub binary.
# Fails the script when cargo fails (LASTEXITCODE check).
param([switch]$Release)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$cfg = if ($Release) { "release" } else { "debug" }
Write-Host "Building bridge ($cfg)..." -ForegroundColor Cyan
# Native stderr must not be fatal under Stop (cargo always writes there);
# the exit code below is the real failure signal.
$nativePref = $ErrorActionPreference
$ErrorActionPreference = "Continue"
cargo build $(if ($Release) { "--release" }) --manifest-path "$root\bridge\Cargo.toml"
$nativeExit = $LASTEXITCODE
$ErrorActionPreference = $nativePref
if ($nativeExit -ne 0) { throw "bridge build failed" }
Write-Host "Bridge built: target\$cfg\cardboard-bridge-svc.exe" -ForegroundColor Green
