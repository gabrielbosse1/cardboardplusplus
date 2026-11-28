# Generates the Vulkan SPIR-V headers for the compose/nv12 shaders with dxc,
# mirroring the Linux shader pipeline in driver_cardboardplusplus/CMakeLists.txt
# (same dxc version, flags and header layout). The Windows .sln has no offline
# shader step, so run this before msbuild — CI does it as a pre-build step and
# scripts/compile-driver.ps1 calls it for local builds. Output: the three
# CBPPSpirv_* headers under driver_cardboardplusplus/include/shaders/spirv.
#
# Never translate DXBC at runtime through dxil-spirv: its BasicResourceMapping
# assigns one descriptor set per resource with binding 0, which never matches
# the single-set pipeline layouts in VulkanCompose, and translating the NV12
# compute shader crashed the process.
param(
    [string]$DxcDir = "",   # Optional: directory that already contains dxc.exe
    [string]$OutDir = ""    # Optional: where the .spv.h headers are written
)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue" # Invoke-WebRequest is glacial with progress enabled
# Any failure below is reported as a ::error:: annotation so the failure reason
# is visible in the check run without downloading job logs.
function Fail([string]$why) {
    "::error::build-spirv: $why"
    exit 1
}
$repoRoot = Split-Path -Parent $PSScriptRoot
$shaderDir = Join-Path $repoRoot "driver_cardboardplusplus\include\shaders"
if (-not $OutDir) { $OutDir = Join-Path $shaderDir "spirv" }

$DxcVersion = "v1.9.2609"   # keep in sync with .github/workflows/linux-driver.yml
$DxcZip = "dxc_2026_09_29.zip"
$DxcUrl = "https://github.com/microsoft/DirectXShaderCompiler/releases/download/$DxcVersion/$DxcZip"

# Locate dxc.exe: explicit -DxcDir, then PATH, then the TEMP download cache.
# Prefer the x64 build: the dxc release zip ships arm/arm64/x64/x86 side by
# side, and an arm64 dxc.exe refuses to run on x64 Windows.
function Find-DxcExe([string]$dir) {
    if (-not $dir -or -not (Test-Path $dir)) { return $null }
    if (Test-Path (Join-Path $dir "dxc.exe") -PathType Leaf) { return (Join-Path $dir "dxc.exe") }
    $all = Get-ChildItem -Path $dir -Recurse -Filter dxc.exe -ErrorAction SilentlyContinue
    $x64 = $all | Where-Object { $_.FullName -match '\\x64\\' } | Select-Object -First 1
    if ($x64) { return $x64.FullName }
    return ($all | Select-Object -First 1).FullName
}
$dxcExe = $null
$candidateDirs = @($DxcDir, (Join-Path $env:TEMP "cbpp_dxc"))
foreach ($dir in $candidateDirs) {
    $dxcExe = Find-DxcExe $dir
    if ($dxcExe) { break }
}
if (-not $dxcExe) {
    $onPath = Get-Command dxc.exe -ErrorAction SilentlyContinue
    if ($onPath) { $dxcExe = $onPath.Source }
}
if (-not $dxcExe) {
    Write-Host "Downloading DirectXShaderCompiler $DxcVersion..."
    $cache = Join-Path $env:TEMP "cbpp_dxc"
    $zip = Join-Path $env:TEMP $DxcZip
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $downloaded = $false
    foreach ($attempt in 1..3) {
        try {
            Invoke-WebRequest -Uri $DxcUrl -OutFile $zip -UseBasicParsing -MaximumRedirection 5
            $downloaded = $true
            break
        } catch {
            Write-Host "download attempt $attempt failed: $($_.Exception.Message)"
            Start-Sleep -Seconds 5
        }
    }
    if (-not $downloaded) { Fail "could not download $DxcUrl after 3 attempts" }
    try {
        if (Test-Path $cache) { Remove-Item -Recurse -Force $cache }
        Expand-Archive -Path $zip -DestinationPath $cache
    } catch {
        Fail "Expand-Archive failed: $($_.Exception.Message)"
    }
    $dxcExe = Find-DxcExe $cache
    if (-not $dxcExe) { Fail "dxc.exe not found in the downloaded archive" }
}
if (-not (Test-Path $dxcExe)) { Fail "dxc.exe not found: $dxcExe" }
Write-Host "dxc: $dxcExe"

# Same profiles and register-shift mapping as the CMake foreach loop. The
# -fvk-*-shift flags are stable across dxc releases; the one-argument
# [[vk::binding(n)]] attribute is not (newer dxc reads it as a SET number).
$profiles = [ordered]@{
    compose_vs = "vs_6_0"
    compose_ps = "ps_6_0"
    nv12_cs = "cs_6_0"
}
$shifts = @{
    compose_vs = ""
    compose_ps = "-fvk-b-shift 0 0 -fvk-s-shift 1 0 -fvk-t-shift 2 0"
    nv12_cs = "-fvk-b-shift 0 0 -fvk-t-shift 1 0 -fvk-u-shift 2 0"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$tmp = Join-Path $env:TEMP "cbpp_spirv_build"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

foreach ($name in $profiles.Keys) {
    $hlsli = Join-Path $shaderDir "$name.hlsli"
    if (-not (Test-Path $hlsli)) { Fail "missing shader source: $hlsli" }
    $text = [IO.File]::ReadAllText($hlsli)
    # Extract the HLSL inside the R"( ... )"; raw string, exactly like CMake does.
    $open = $text.IndexOf('R"(')
    if ($open -lt 0) { Fail "no raw string found in $hlsli" }
    $start = $open + 3
    $close = $text.IndexOf(')";', $start)
    if ($close -lt 0 -or $close -lt $start) { Fail "unterminated raw string in $hlsli" }
    $hlslFile = Join-Path $tmp "$name.hlsl"
    [IO.File]::WriteAllText($hlslFile, $text.Substring($start, $close - $start))
    $spvFile = Join-Path $tmp "$name.spv"

    Write-Host "dxc $name ($($profiles[$name])) -> SPIR-V"
    # Launch dxc through cmd.exe with file redirection. PowerShell's native
    # launcher on GH Actions runners throws "StandardOutputEncoding is only
    # supported when standard output is redirected" when a script's output is
    # piped; cmd.exe performs the launch natively and captures dxc's output to
    # a log file, sidestepping that entirely. Exit code is cmd's = dxc's.
    $dxcLog = Join-Path $tmp "$name.dxc.log"
    $dxcCmd = "`"$dxcExe`" -spirv -T $($profiles[$name]) -E main -fspv-target-env=vulkan1.1" +
              " $($shifts[$name]) -O3 `"$hlslFile`" -Fo `"$spvFile`" > `"$dxcLog`" 2>&1"
    cmd /c $dxcCmd
    $dxcExit = $LASTEXITCODE
    if (Test-Path $dxcLog) {
        Get-Content $dxcLog | ForEach-Object { if ($_) { Write-Host "dxc: $_" } }
    }
    if ($dxcExit -ne 0) {
        if (Test-Path $dxcLog) {
            Get-Content $dxcLog | Select-Object -First 8 |
                ForEach-Object { if ($_) { "::error::dxc ${name}: $_" } }
        }
        Fail "dxc failed for $name (exit $dxcExit)"
    }

    # Embed as a C header with little-endian uint32 words, the same layout
    # cmake/EmbedSpirv.cmake produces for the Linux build.
    $bytes = [IO.File]::ReadAllBytes($spvFile)
    if ($bytes.Length -eq 0 -or ($bytes.Length % 4) -ne 0) { Fail "invalid .spv size for $name ($($bytes.Length) bytes)" }
    # Keep the raw SPIR-V next to the header for diagnostics.
    [IO.File]::WriteAllBytes((Join-Path $OutDir "$name.spv"), $bytes)
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine("// Generated by scripts/build-spirv.ps1 from $name.hlsli (dxc $DxcVersion). Do not edit.")
    [void]$sb.AppendLine("#pragma once")
    [void]$sb.AppendLine("#include <cstdint>")
    [void]$sb.AppendLine("static const uint32_t CBPPSpirv_$name[] = {")
    $wordCount = $bytes.Length / 4
    for ($i = 0; $i -lt $bytes.Length; $i += 4) {
        $w = [long]$bytes[$i] -bor ([long]$bytes[$i + 1] -shl 8) -bor ([long]$bytes[$i + 2] -shl 16) -bor ([long]$bytes[$i + 3] -shl 24)
        [void]$sb.Append(("0x{0:x8}u, " -f $w))
        if ((($i / 4) % 8) -eq 7) { [void]$sb.AppendLine() }
    }
    [void]$sb.AppendLine()
    [void]$sb.AppendLine("};")
    $hdr = Join-Path $OutDir "$name.spv.h"
    [IO.File]::WriteAllText($hdr, $sb.ToString())
    Write-Host "wrote $hdr ($wordCount words)"
}
Write-Host "SPIR-V headers ready in $OutDir"
