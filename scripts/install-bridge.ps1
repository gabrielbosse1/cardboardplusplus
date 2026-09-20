# Installs the bridge hub (needs admin): copies the exe + MediaPipe sidecar +
# hand model into Program Files\CardboardPlusPlus, adds a Start Menu shortcut,
# warns when ffmpeg/Python are missing (preview/hands stay off), and opens
# the inbound UDP firewall for telemetry (42071) and camera (42072).
param([switch]$Release)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$cfg = if ($Release) { "release" } else { "debug" }
$crate = Join-Path $root "bridge\crates\cardboard-bridge"
if (-not [Environment]::Is64BitProcess) { throw "run 64-bit PowerShell (System32, not SysWOW64)" }
$dest = Join-Path $env:ProgramFiles "CardboardPlusPlus"
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { throw "needs admin (Program Files + firewall) -- re-run PowerShell as administrator" }
$exe = Join-Path $root "bridge\target\$cfg\cardboard-bridge-svc.exe"
if (-not (Test-Path -LiteralPath $exe)) { throw "build the bridge first: scripts\compile-bridge.ps1" }
New-Item -ItemType Directory -Path $dest -Force | Out-Null
Copy-Item -LiteralPath $exe -Destination $dest -Force
Copy-Item -LiteralPath (Join-Path $crate "mediapipe_server.py") -Destination $dest -Force
$modelDst = Join-Path $dest "models"
New-Item -ItemType Directory -Path $modelDst -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $crate "models\hand_landmarker.task") -Destination $modelDst -Force
foreach ($f in @((Join-Path $dest "cardboard-bridge-svc.exe"), (Join-Path $dest "mediapipe_server.py"), (Join-Path $modelDst "hand_landmarker.task"))) {
  if (-not (Test-Path -LiteralPath $f)) { throw "missing after install: $f" }
}
$group = Join-Path ([Environment]::GetFolderPath("CommonStartMenu")) "Programs\Cardboard++"
New-Item -ItemType Directory -Path $group -Force | Out-Null
$sc = (New-Object -ComObject WScript.Shell).CreateShortcut((Join-Path $group "Cardboard++ Bridge.lnk"))
$sc.TargetPath = Join-Path $dest "cardboard-bridge-svc.exe"
$sc.WorkingDirectory = $dest
$sc.Save()
try { $ff = (ffmpeg -version 2>$null | Select-Object -First 1) } catch { $ff = $null }
if (-not $ff) { Write-Warning "ffmpeg not on PATH -- preview stays dark. Install: winget install ffmpeg" }
$py = $null
foreach ($c in @(@{p="py";a=@("-3")}, @{p="python";a=@()})) {
  try { & $c.p @($c.a) --version 2>$null | Out-Null; if ($LASTEXITCODE -eq 0) { $py = $c; break } } catch {}
}
if (-not $py) { Write-Warning "no Python found -- hand tracking disabled. Then: py -3 -m pip install -r $crate\requirements.txt" }
else { Write-Host ("python: " + ((& $py.p @($py.a) --version 2>&1) | Select-Object -First 1)) }
foreach ($port in @(42071, 42072)) {
  $name = "Cardboard++ Bridge UDP $port"
  if (-not (Get-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue)) {
    New-NetFirewallRule -DisplayName $name -Direction Inbound -Protocol UDP -LocalPort $port -Action Allow | Out-Null
    Write-Host "firewall: allowed inbound UDP $port"
  }
}
Write-Host "Bridge installed in $dest" -ForegroundColor Green
