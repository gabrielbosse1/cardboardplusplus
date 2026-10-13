# Full Windows e2e automation, one command:
#   1. live pass  - starts SteamVR + the bridge if needed, drives the real
#                   driver over REST, then tears the stack back down so the
#                   fixed UDP ports are free.
#   2. fake pass  - real bridge binary + fake driver/phone/sidecar, serial.
# -FakeOnly skips the live pass. Requires the driver installed
#   (scripts/install-driver.ps1) for the live pass.
param(
    [switch]$FakeOnly
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$manifest = Join-Path $repo "bridge\Cargo.toml"
$steamvr = "C:\Program Files (x86)\Steam\steamapps\common\SteamVR"
$bridgeExe = Join-Path $repo "bridge\target\debug\cardboard-bridge-svc.exe"

function Get-BridgeStatus {
    try {
        $port = if ($env:CARDBOARD_BRIDGE_PORT) { $env:CARDBOARD_BRIDGE_PORT } else { 8567 }
        return Invoke-RestMethod -Uri "http://127.0.0.1:$port/status" -TimeoutSec 2
    } catch {
        return $null
    }
}
function Wait-LiveStack([int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        $s = Get-BridgeStatus
        if ($s -and $s.driver_connected -eq $true -and $s.preview_driver_fps -gt 0) { return $true }
        Start-Sleep 2
    }
    return $false
}
function Stop-LiveStack {
    Get-Process cardboard-bridge-svc -ErrorAction SilentlyContinue | Stop-Process -Force
    Get-CimInstance Win32_Process -Filter "Name like '%python%'" |
        Where-Object { $_.CommandLine -like "*mediapipe_server.py*" } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    $vrserver = Join-Path $steamvr "bin\win64\vrserver.exe"
    if ((Get-Process vrserver -ErrorAction SilentlyContinue) -and (Test-Path $vrserver)) {
        Start-Process $vrserver -ArgumentList "--quit" -WindowStyle Hidden
    }
    $deadline = (Get-Date).AddSeconds(40)
    while ((Get-Process vrserver -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) { Start-Sleep 1 }
    Get-Process vrserver,vrmonitor,vrcompositor -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 2
}

cargo build --manifest-path $manifest --bin cardboard-bridge-svc
if ($LASTEXITCODE -ne 0) { Write-Host "bridge build FAILED"; exit $LASTEXITCODE }
cargo test --manifest-path $manifest --test e2e --no-run
if ($LASTEXITCODE -ne 0) { Write-Host "e2e test build FAILED"; exit $LASTEXITCODE }

if (-not $FakeOnly) {
    Write-Host "== live pass: real bridge + installed SteamVR driver =="
    if (-not (Get-Process vrserver -ErrorAction SilentlyContinue)) {
        $vrstartup = Join-Path $steamvr "bin\win64\vrstartup.exe"
        if (-not (Test-Path $vrstartup)) {
            Write-Host "SteamVR not found at $steamvr - cannot run live pass"
            exit 1
        }
        if (-not (Get-Process steam -ErrorAction SilentlyContinue)) {
            $steamExe = Join-Path (($steamvr -split "\\steamapps\\")[0]) "Steam.exe"
            if (Test-Path $steamExe) {
                Write-Host "starting Steam client (SteamVR needs it)"
                Start-Process $steamExe
                Start-Sleep 30
            }
        }
        Start-Process $vrstartup
    }
    if (-not (Get-BridgeStatus)) {
        if (-not (Test-Path $bridgeExe)) { Write-Host "bridge exe missing: $bridgeExe"; exit 1 }
        Start-Process $bridgeExe -ArgumentList "--headless" -WindowStyle Hidden
    }
    if (-not (Wait-LiveStack 150)) {
        Write-Host "live stack never came up (driver_connected + streaming) - install the driver (scripts\install-driver.ps1) and check SteamVR"
        exit 1
    }
    $env:CBPP_E2E_LIVE = "1"
    cargo test --manifest-path $manifest --test e2e -- --test-threads=1 live_bridge
    $code = $LASTEXITCODE
    Remove-Item Env:CBPP_E2E_LIVE -ErrorAction SilentlyContinue
    if ($code -ne 0) { Write-Host "live pass FAILED (exit $code)"; exit $code }
    Write-Host "live pass OK - tearing the stack down to free the wire ports"
    Stop-LiveStack
}

Write-Host "== fake pass: real bridge binary + fake driver/phone/sidecar (serial) =="
cargo test --manifest-path $manifest --test e2e -- --test-threads=1
if ($LASTEXITCODE -ne 0) { Write-Host "fake pass FAILED (exit $LASTEXITCODE)"; exit $LASTEXITCODE }
Write-Host "e2e passed (live + fake). Artifacts: bridge\target\tmp\*\e2e-artifacts"
