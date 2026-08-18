param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('driver', 'android', 'bridge')]
    [string]$Module,
    [ValidateSet('baseline', 'check')]
    [string]$Mode = 'check'
)

$ErrorActionPreference = 'Continue'

# ---------------------------------------------------------------- paths
$harnessDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$agentizeDir = Split-Path -Parent $harnessDir
$repoRoot = Split-Path -Parent $agentizeDir
$sandbox = Join-Path $agentizeDir ("work\{0}" -f $Module)
$resDir = Join-Path $harnessDir ("results\{0}" -f $Module)
$outDir = Join-Path $resDir $Mode

if (-not (Test-Path $sandbox)) {
    Write-Host "[harness $Module] FATAL: sandbox missing: $sandbox -- run agentize\setup.ps1 first" -ForegroundColor Red
    exit 2
}
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

function Step { param([string]$s) Write-Host "[$Module/$Mode] $s" -ForegroundColor Cyan }
function Fail  { param([string]$s) Write-Host "[$Module/$Mode] FAIL: $s" -ForegroundColor Red }
function Pass  { param([string]$s) Write-Host "[$Module/$Mode] PASS: $s" -ForegroundColor Green }

$failures = 0
function RequireEqual {
    param([string]$Name, [string]$BaselinePath, [string]$CheckPath, [switch]$Sorted)
    if (-not (Test-Path $BaselinePath)) { Fail "[$Name] baseline missing: $BaselinePath"; $script:failures++ ; return }
    if (-not (Test-Path $CheckPath))    { Fail "[$Name] artifact missing after check: $CheckPath"; $script:failures++ ; return }
    $a = Get-Content -LiteralPath $BaselinePath -Raw
    $b = Get-Content -LiteralPath $CheckPath -Raw
    if ($Sorted) { $a = (($a -split "`n") | Sort-Object) -join "`n"; $b = (($b -split "`n") | Sort-Object) -join "`n" }
    if ($a -ceq $b) { Pass "[$Name] identical" } else {
        Fail "[$Name] differs (baseline vs check)."
        $diff = Compare-Object ($a -split "`n") ($b -split "`n") -ErrorAction SilentlyContinue
        if ($diff) { $diff | Select-Object -First 20 | ForEach-Object { Write-Host ("      {0} {1}" -f $_.SideIndicator, $_.InputObject) -ForegroundColor Yellow } }
        $script:failures++
    }
}

# ---------------------------------------------------------------- tools
function Get-VisualStudioRoot {
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $p = & $vswhere -latest -products * -property installationPath 2>$null
        if ($p -is [array]) { $p = $p | Select-Object -First 1 }
        if ($p) { return ($p.Trim()) }
    }
    return $null
}
function Get-MSBuild {
    $vs = Get-VisualStudioRoot
    if ($vs) {
        $cand = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'
        if (Test-Path $cand) { return $cand }
    }
    return $null
}
function Get-Dumpbin {
    $vs = Get-VisualStudioRoot
    if ($vs) {
        $found = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Recurse -Filter dumpbin.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -like '*Hostx64\x64\dumpbin.exe' } |
            Sort-Object FullName -Descending | Select-Object -First 1
        if ($found) { return $found.FullName }
    }
    return $null
}
function Get-Aapt {
    foreach ($root in @($env:ANDROID_HOME, $env:ANDROID_SDK_ROOT)) {
        if (-not $root) { continue }
        $found = Get-ChildItem (Join-Path $root 'build-tools') -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending | ForEach-Object {
                $c = Join-Path $_.FullName 'aapt.exe'
                if (Test-Path $c) { return $c }
            } | Select-Object -First 1
        if ($found) { return $found }
    }
    return $null
}
function Get-ErrorsFromText {
    param([string]$Text)
    return @(($Text -split "`r?`n") | Where-Object { $_ -match ':\s*error\s' }) 
}

# ---------------------------------------------------------------- driver
function Invoke-Driver {
    $msbuild = Get-MSBuild
    $dumpbin = Get-Dumpbin
    if (-not $msbuild -or -not $dumpbin) {
        Fail "Visual Studio toolset not found (need MSBuild.exe + dumpbin.exe). Install VC++ workload (vswhere)."
        exit 2
    }
    Step "building Release|x64 (msbuild)"
    $buildLog = Join-Path $outDir 'build.log'
    $tmp = Join-Path $env:TEMP ("{0}.log" -f [guid]::NewGuid().ToString('N'))
    & $msbuild (Join-Path $sandbox 'driver_cardboardplusplus.sln') /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal *> $tmp
    $buildExit = $LASTEXITCODE
    $out = Get-Content -LiteralPath $tmp -Raw
    Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    $out | Out-File -FilePath $buildLog -Encoding utf8
    $errors = Get-ErrorsFromText $out
    if ($buildExit -ne 0 -or $errors.Count -gt 0) {
        Fail "build FAILED ($($errors.Count) errors, exit $buildExit). See result build.log"
        $errors | Select-Object -First 10 | ForEach-Object { Write-Host ("      " + $_) -ForegroundColor Yellow }
        exit 1
    }
    Pass "build clean (0 errors)"

    $dll = Join-Path $sandbox 'x64\Release\driver_cardboardplusplus.dll'
    if (-not (Test-Path $dll)) {
        Fail "expected DLL not produced: $dll"
        exit 1
    }

    $exports = Join-Path $outDir 'exports.txt'
    $sym = (& $dumpbin /EXPORTS $dll 2>$null |
        ForEach-Object { if ($_ -match '^\s*\d+\s+\d+\s+[0-9A-Fa-f]+\s+(\S+)') { $Matches[1] } } |
        Sort-Object -Unique) -join "`n"
    $sym | Out-File -FilePath $exports -Encoding ascii

    $resHashes = @()
    foreach ($rel in @('resources\driver.vrdrivermanifest', 'resources\controller_profile.json')) {
        $f = Join-Path $sandbox $rel
        if (Test-Path $f) {
            $h = (Get-FileHash -LiteralPath $f -Algorithm SHA256).Hash
            $resHashes += ("{0}  {1}" -f $h, $rel)
        }
    }
    $resHashes -join "`n" | Out-File -FilePath (Join-Path $outDir 'resources.sha256') -Encoding ascii

    if ($Mode -eq 'check') {
        RequireEqual "DLL exports" (Join-Path $resDir 'baseline\exports.txt') $exports -Sorted
        RequireEqual "resource files" (Join-Path $resDir 'baseline\resources.sha256') (Join-Path $outDir 'resources.sha256') -Sorted
    }
}

# ---------------------------------------------------------------- android
function Invoke-Android {
    $aapt = Get-Aapt
    if (-not $aapt) {
        Fail "Android SDK not found. Set ANDROID_HOME (with build-tools) or provide local.properties at the android sandbox root, then re-run setup.ps1."
        exit 2
    }
    $gradlew = Join-Path $sandbox 'gradlew.bat'
    if (-not (Test-Path $gradlew)) {
        Fail "gradlew.bat missing in sandbox: $gradlew"
        exit 2
    }
    Step "building :app:assembleDebug (gradle)"
    $buildLog = Join-Path $outDir 'build.log'
    $tmp = Join-Path $env:TEMP ("{0}.log" -f [guid]::NewGuid().ToString('N'))
    & $gradlew -p $sandbox :app:assembleDebug --no-daemon --console=plain *> $tmp
    $buildExit = $LASTEXITCODE
    $out = Get-Content -LiteralPath $tmp -Raw
    Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    $out | Out-File -FilePath $buildLog -Encoding utf8
    $errors = Get-ErrorsFromText $out
    if ($buildExit -ne 0 -or $errors.Count -gt 0) {
        Fail "build FAILED ($($errors.Count) errors, exit $buildExit). See build.log"
        $errors | Select-Object -First 10 | ForEach-Object { Write-Host ("      " + $_) -ForegroundColor Yellow }
        exit 1
    }
    Pass "build clean (0 errors)"

    $apk = Join-Path $sandbox 'cardboardplusplus-android\build\outputs\apk\debug\app-debug.apk'
    if (-not (Test-Path $apk)) {
        Fail "expected APK not produced: $apk"
        exit 1
    }

    $badging = Join-Path $outDir 'badging.txt'
    $tmp = Join-Path $env:TEMP ("{0}.log" -f [guid]::NewGuid().ToString('N'))
    & $aapt dump badging $apk *> $tmp
    Get-Content -LiteralPath $tmp -Raw | Out-File -FilePath $badging -Encoding ascii
    Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue

    if ($Mode -eq 'check') {
        RequireEqual "merged APK contract (badging)" (Join-Path $resDir 'baseline\badging.txt') $badging -Sorted
    }
}

# ---------------------------------------------------------------- bridge
function Invoke-Bridge {
    if (-not (Get-Command cargo -ErrorAction SilentlyContinue)) {
        Fail "Rust toolchain (cargo) not found on PATH."
        exit 2
    }
    Step "building release (cargo)"
    $buildLog = Join-Path $outDir 'build.log'
    $tmp = Join-Path $env:TEMP ("{0}.log" -f [guid]::NewGuid().ToString('N'))
    & cargo build --release --manifest-path (Join-Path $sandbox 'Cargo.toml') *> $tmp
    $buildExit = $LASTEXITCODE
    $out = Get-Content -LiteralPath $tmp -Raw
    Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    $out | Out-File -FilePath $buildLog -Encoding utf8
    $errors = Get-ErrorsFromText $out
    if ($buildExit -ne 0 -or $errors.Count -gt 0) {
        Fail "build FAILED ($($errors.Count) errors, exit $buildExit). See build.log"
        $errors | Select-Object -First 10 | ForEach-Object { Write-Host ("      " + $_) -ForegroundColor Yellow }
        exit 1
    }
    Pass "build clean (0 errors)"

    $exe = Join-Path $sandbox 'target\release\cardboard-bridge.exe'
    if (-not (Test-Path $exe)) {
        Fail "expected binary not produced: $exe"
        exit 1
    }

    Step "replaying scripted session (mock driver + mock phone + REST)"
    $transcript = Join-Path $outDir 'transcript.txt'
    $mock = Join-Path $harnessDir 'bridge_mock.ps1'
    $tmp = Join-Path $env:TEMP ("{0}.log" -f [guid]::NewGuid().ToString('N'))
    & powershell -NoProfile -ExecutionPolicy Bypass -File $mock -BridgeExe $exe -Out $transcript *> $tmp
    $sessionExit = $LASTEXITCODE
    $sessionOut = Get-Content -LiteralPath $tmp -Raw
    Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    if ($sessionExit -ne 0) {
        Fail "session replay FAILED ($sessionExit). $sessionOut"
        exit 1
    }
    Pass "session replayed"

    if ($Mode -eq 'check') {
        RequireEqual "runtime transcript" (Join-Path $resDir 'baseline\transcript.txt') $transcript
    }
}

# ---------------------------------------------------------------- run
switch ($Module) {
    'driver'  { Invoke-Driver }
    'android' { Invoke-Android }
    'bridge'  { Invoke-Bridge }
}

if ($failures -gt 0) {
    Fail ("{0} equivalence gate(s) FAILED for module '{1}' (mode {2})" -f $failures, $Module, $Mode)
    Write-Host "NOT DONE. Keep refactoring until all gates pass."
    exit 1
}
Pass ("ALL GATES PASS for '{0}' (mode {1})" -f $Module, $Mode)
if ($Mode -eq 'check') {
    Write-Host "[$Module] DONE - behavior proven unchanged."
}
exit 0
