# Installs the compiled driver into SteamVR: checks the DLL, SteamVR dir,
# FFmpeg runtime, and VC++ redist; backs up the existing DLL; copies DLL +
# FFmpeg + manifest/resources with locked-file retries (quit SteamVR first);
# -RefreshDeps purges stale FFmpeg majors. Restart SteamVR to load it.
param([string]$SteamVR = "C:\Program Files (x86)\Steam\steamapps\common\SteamVR",
      [switch]$RefreshDeps)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot\ffmpeg-deps.ps1"
$ffmpegDir = "$root\driver_cardboardplusplus\lib\ffmpeg"
$srcDll = "$root\driver_cardboardplusplus\x64\Release\driver_cardboardplusplus.dll"
$srcFfmpeg = Join-Path $ffmpegDir "bin"
$srcResources = "$root\driver_cardboardplusplus\resources"
$dstDir = "$SteamVR\drivers\cardboardplusplus\bin\win64"
$dstRoot = "$SteamVR\drivers\cardboardplusplus"
if (-not (Test-Path $srcDll)) {
    throw "Driver DLL not found at $srcDll - run scripts\compile-driver.ps1 first"
}
if (-not (Test-Path "$SteamVR\drivers")) {
    throw "SteamVR drivers directory not found at $SteamVR\drivers - check -SteamVR path or install SteamVR"
}
Ensure-FfmpegDeps $ffmpegDir
$requiredFfmpeg = Get-FfmpegDepDlls $ffmpegDir
$vcRedist = Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64" -ErrorAction SilentlyContinue
if (-not $vcRedist -or $vcRedist.Installed -ne 1) {
    throw "VC++ 2015-2022 x64 redistributable not detected - install it from https://aka.ms/vs/17/release/vc_redist.x64.exe then re-run"
}
$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Write-Warning "Not running as Administrator - install may fail writing to $dstDir" }
Write-Host "Installing driver to $dstDir ..." -ForegroundColor Cyan
New-Item -ItemType Directory -Path $dstDir -Force | Out-Null
$dstDll = Join-Path $dstDir "driver_cardboardplusplus.dll"
if (Test-Path $dstDll) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $bak = "$dstDll.bak.$stamp"
    Copy-Item $dstDll $bak -Force
    Write-Host "Backed up existing DLL to $bak" -ForegroundColor Yellow
}
# Copies with 4 attempts / 2s pauses: SteamVR locks the DLL while running,
# so a first-try failure usually means "quit SteamVR and re-run".
function Copy-WithRetry([string]$src, [string]$dst, [int]$attempts = 4) {
    for ($i = 1; $i -le $attempts; $i++) {
        try {
            Copy-Item $src $dst -Force -ErrorAction Stop
            return
        } catch {
            if ($i -eq $attempts) {
                throw "Copy $src -> $dst failed after $attempts attempts (is SteamVR/vrserver.exe still running? Quit SteamVR and re-run): $($_.Exception.Message)"
            }
            Start-Sleep -Seconds 2
        }
    }
}
Copy-WithRetry $srcDll $dstDir
if ($RefreshDeps) {
    foreach ($pat in @("av*.dll", "sw*.dll")) {
        Get-ChildItem -LiteralPath $dstDir -Filter $pat -File -ErrorAction SilentlyContinue |
            Remove-Item -Force -ErrorAction Stop
    }
    Write-Host "RefreshDeps: removed stale FFmpeg DLLs from $dstDir" -ForegroundColor Yellow
}
$ffmpegDlls = @(foreach ($name in $requiredFfmpeg) { Join-Path $srcFfmpeg $name })
foreach ($dll in $ffmpegDlls) { Copy-WithRetry $dll $dstDir }
Write-Host ("Copied {0} FFmpeg DLL(s): {1}" -f $ffmpegDlls.Count, ($requiredFfmpeg -join ", "))
$manifestSrc = Join-Path $srcResources "driver.vrdrivermanifest"
if (-not (Test-Path $manifestSrc)) {
    throw "Driver manifest not found at $manifestSrc"
}
New-Item -ItemType Directory -Path $dstRoot -Force | Out-Null
Copy-WithRetry $manifestSrc $dstRoot
$dstRes = Join-Path $dstRoot "resources"
New-Item -ItemType Directory -Path $dstRes -Force | Out-Null
$resFiles = Get-ChildItem (Join-Path $srcResources "*")
foreach ($f in $resFiles) { Copy-WithRetry $f.FullName $dstRes }
Write-Host ("Copied {0} resource file(s) to {1}" -f $resFiles.Count, $dstRes)
Write-Host "Driver installed to $dstDir" -ForegroundColor Green
Write-Host "Restart SteamVR to load the driver." -ForegroundColor Yellow
