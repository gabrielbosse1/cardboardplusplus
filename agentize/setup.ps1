param(
    [switch]$IncludeAndroid   # run the Android baseline too (needs Android SDK on this machine)
)

$ErrorActionPreference = 'Stop'

$agentizeDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot    = Split-Path -Parent $agentizeDir
$work        = Join-Path $agentizeDir 'work'
$verify      = Join-Path $agentizeDir 'harness\verify.ps1'

function Copy-Sandbox {
    param([string]$Name, [string]$Source, [string[]]$ExcludeDirs)
    $dest = Join-Path $work $Name
    if (Test-Path $dest) { Remove-Item -LiteralPath $dest -Recurse -Force }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
    $args = @($Source, $dest, '/E', '/XJ', '/NFL', '/NDL', '/NJH', '/NJS', '/NP')
    foreach ($d in $ExcludeDirs) { $args += @('/XD', $d) }
    & robocopy @args | Out-Null
    $code = $LASTEXITCODE
    $script:lastRobo = $code
    if ($code -ge 8) { throw "robocopy failed for $Name (exit $code)" }
    Write-Host "sandbox ready: $dest"
}

# ---------------------------------------------------------------- sandboxes
Write-Host "== Creating per-module sandboxes (private parallel work copies) =="

Copy-Sandbox 'driver'  (Join-Path $repoRoot 'driver_cardboardplusplus')  @('x64', 'driver_c.e9f07c0e', '.git')
Copy-Sandbox 'bridge'  (Join-Path $repoRoot 'bridge')                    @('target', '.git')

# Android sandbox = gradle root + sdk + app module
$android = Join-Path $work 'android'
if (Test-Path $android) { Remove-Item -LiteralPath $android -Recurse -Force }
New-Item -ItemType Directory -Force -Path $android | Out-Null
foreach ($f in @('settings.gradle', 'build.gradle', 'gradle.properties', 'gradlew', 'gradlew.bat', 'local.properties')) {
    $src = Join-Path $repoRoot $f
    if (Test-Path $src) { Copy-Item -LiteralPath $src -Destination $android -Force }
}
Copy-Sandbox '__androidsdk' (Join-Path $repoRoot 'sdk') @('build', '.gradle', '.idea')
if (Test-Path (Join-Path $work '__androidsdk')) { Move-Item (Join-Path $work '__androidsdk') (Join-Path $android 'sdk') -Force }
Copy-Sandbox '__androidapp' (Join-Path $repoRoot 'cardboardplusplus-android') @('build', '.gradle', '.idea', 'libraries')
if (Test-Path (Join-Path $work '__androidapp')) { Move-Item (Join-Path $work '__androidapp') (Join-Path $android 'cardboardplusplus-android') -Force }
Write-Host "sandbox ready: $android"

# ---------------------------------------------------------------- baselines
Write-Host "== Recording baselines (pristine behavior fingerprints) =="

foreach ($module in @('driver', 'bridge')) {
    Write-Host "--- baseline: $module ---"
    & powershell -NoProfile -ExecutionPolicy Bypass -File $verify -Module $module -Mode baseline
    if ($LASTEXITCODE -ne 0) { Write-Host "WARN: baseline for '$module' did not record cleanly (exit $LASTEXITCODE)" }
}

if ($IncludeAndroid) {
    Write-Host "--- baseline: android ---"
    & powershell -NoProfile -ExecutionPolicy Bypass -File $verify -Module android -Mode baseline
    if ($LASTEXITCODE -ne 0) { Write-Host "WARN: baseline for 'android' did not record cleanly (exit $LASTEXITCODE)" }
} else {
    Write-Host "(android baseline skipped - pass -IncludeAndroid once the Android SDK is configured)"
}

Write-Host ""
Write-Host "Setup done. Agents can now start. Stop-gate for each agent:"
Write-Host "  powershell -File agentize\harness\verify.ps1 -Module <name> -Mode check"