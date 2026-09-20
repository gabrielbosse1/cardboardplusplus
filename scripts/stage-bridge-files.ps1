param([switch]$Release, [switch]$SkipPython)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$cfg = if ($Release) { "release" } else { "debug" }
$crate = Join-Path $root "bridge\crates\cardboard-bridge"
$stage = Join-Path $root "installer\stage"
$outDir = Join-Path $root "installer\dist"
$pyVersion = "3.11.9"
$pyUrl = "https://www.python.org/ftp/python/$pyVersion/python-$pyVersion-embed-amd64.zip"
& "$PSScriptRoot\compile-bridge.ps1" -Release:$Release
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $root "bridge\target\$cfg\cardboard-bridge-svc.exe") -Destination $stage -Force
Copy-Item -LiteralPath (Join-Path $crate "mediapipe_server.py") -Destination $stage -Force
New-Item -ItemType Directory -Path (Join-Path $stage "models") -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $crate "models\hand_landmarker.task") -Destination (Join-Path $stage "models") -Force
$pyDir = Join-Path $stage "python"
if (-not $SkipPython) {
  $tmp = Join-Path ([System.IO.Path]::GetTempPath()) "cb-pyembed-$pyVersion"
  if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force }
  New-Item -ItemType Directory -Path $tmp -Force | Out-Null
  $zip = Join-Path $tmp "python-embed.zip"
  Write-Host "Downloading embedded Python $pyVersion..." -ForegroundColor Cyan
  & curl.exe -fSL --retry 3 -o $zip $pyUrl
  if ($LASTEXITCODE -ne 0) { throw "Python embed download failed: $pyUrl" }
  New-Item -ItemType Directory -Path $pyDir -Force | Out-Null
  Expand-Archive -LiteralPath $zip -DestinationPath $pyDir -Force
  $pth = Get-ChildItem -LiteralPath $pyDir -Filter "python3*._pth" | Select-Object -First 1
  $text = Get-Content -LiteralPath $pth.FullName -Raw
  $text = $text -replace "#import site", "import site"
  if ($text -notmatch "Lib\\\\site-packages") { $text = $text.TrimEnd() + "`r`nLib\site-packages`r`n" }
  Set-Content -LiteralPath $pth.FullName -Value $text -NoNewline
  Write-Host "Bootstrapping pip..." -ForegroundColor Cyan
  & curl.exe -fSL --retry 3 -o (Join-Path $tmp "get-pip.py") https://bootstrap.pypa.io/get-pip.py
  if ($LASTEXITCODE -ne 0) { throw "get-pip.py download failed" }
  & (Join-Path $pyDir "python.exe") (Join-Path $tmp "get-pip.py") --no-warn-script-location
  if ($LASTEXITCODE -ne 0) { throw "get-pip failed" }
  Write-Host "Installing sidecar deps (mediapipe/opencv/numpy, one-time)..." -ForegroundColor Cyan
  & (Join-Path $pyDir "python.exe") -m pip install --no-warn-script-location -r (Join-Path $crate "requirements.txt")
  if ($LASTEXITCODE -ne 0) { throw "pip install requirements failed" }
  Remove-Item -LiteralPath $tmp -Recurse -Force
} elseif (-not (Test-Path -LiteralPath (Join-Path $pyDir "python.exe"))) {
  throw "no staged python -- rerun without -SkipPython once"
}
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$outZip = Join-Path $outDir "bridge-files.zip"
if (Test-Path -LiteralPath $outZip) { Remove-Item -LiteralPath $outZip -Force }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $outZip -Force
Write-Host "Payload staged: $outZip" -ForegroundColor Green
