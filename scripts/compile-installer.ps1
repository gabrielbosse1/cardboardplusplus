param([switch]$Release, [switch]$SkipBridge)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$outDir = Join-Path $root "installer\dist"
if (-not $SkipBridge) {
  & "$PSScriptRoot\compile-bridge.ps1" -Release:$Release
}
$ver = (Select-String -LiteralPath (Join-Path $root "bridge\Cargo.toml") -Pattern '^version = "([^"]+)"' |
  Select-Object -First 1).Matches[0].Groups[1].Value
$iscc = "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
if (-not (Test-Path -LiteralPath $iscc)) {
  $iscc = (Get-Command ISCC.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1)
}
if (-not $iscc) { throw "Inno Setup 6 not found -- winget install --id JRSoftware.InnoSetup -e" }
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
& $iscc (Join-Path $root "installer\bridge.iss") "/DAppVersion=$ver" "/DOutputDir=$outDir"
if ($LASTEXITCODE -ne 0) { throw "ISCC failed" }
Write-Host "Setup built: $outDir\CardboardBridge-Setup-$ver.exe" -ForegroundColor Green
