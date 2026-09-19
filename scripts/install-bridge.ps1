# install-bridge.ps1 — Stage the bridge next to its sidecar + model, check deps.
param([switch]$Release)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$cfg = if ($Release) { "release" } else { "debug" }
$target = Join-Path $root "bridge\target\$cfg"
$crate = Join-Path $root "bridge\crates\cardboard-bridge"

$exe = Join-Path $target "cardboard-bridge.exe"
if (-not (Test-Path -LiteralPath $exe)) { throw "build the bridge first: scripts\compile-bridge.ps1" }

# Sidecar + model live next to the exe (that is where the bridge looks first).
Copy-Item -LiteralPath (Join-Path $crate "mediapipe_server.py") -Destination $target -Force
$modelSrc = Join-Path $crate "models"
$modelDst = Join-Path $target "models"
New-Item -ItemType Directory -Path $modelDst -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $modelSrc "hand_landmarker.task") -Destination $modelDst -Force

# Assert presence — a missing sidecar/model means silent no-tracking.
foreach ($f in @($exe, (Join-Path $target "mediapipe_server.py"), (Join-Path $modelDst "hand_landmarker.task"))) {
  if (-not (Test-Path -LiteralPath $f)) { throw "missing after install: $f" }
}

# Dependency checks (warn only — the bridge logs these at startup too).
try { $ff = (ffmpeg -version 2>$null | Select-Object -First 1) } catch { $ff = $null }
if (-not $ff) { Write-Warning "ffmpeg not on PATH — preview stays dark. Install: winget install ffmpeg" }
$py = $null
foreach ($c in @(@{p="py";a=@("-3")}, @{p="python";a=@()})) {
  try { & $c.p @($c.a) --version 2>$null | Out-Null; if ($LASTEXITCODE -eq 0) { $py = $c; break } } catch {}
}
if (-not $py) { Write-Warning "no Python found — hand tracking disabled. Then: py -3 -m pip install -r $crate\requirements.txt" }
else { Write-Host ("python: " + ((& $py.p @($py.a) --version 2>&1) | Select-Object -First 1)) }

# Firewall: inbound UDP for phone telemetry (42071) + camera (42072).
foreach ($port in @(42071, 42072)) {
  $name = "Cardboard++ Bridge UDP $port"
  if (-not (Get-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue)) {
    New-NetFirewallRule -DisplayName $name -Direction Inbound -Protocol UDP -LocalPort $port -Action Allow | Out-Null
    Write-Host "firewall: allowed inbound UDP $port"
  }
}
Write-Host "Bridge staged in $target" -ForegroundColor Green
