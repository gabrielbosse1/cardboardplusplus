# ffmpeg-deps.ps1 — Shared reader for driver_cardboardplusplus/lib/ffmpeg/deps.json
# (the single source of truth for the driver's FFmpeg runtime DLL versions).
# Dot-source from compile-driver.ps1 / install-driver.ps1:
#   . "$PSScriptRoot\ffmpeg-deps.ps1"

function Get-FfmpegDepsManifest([string]$FfmpegDir) {
    $manifest = Join-Path $FfmpegDir "deps.json"
    if (-not (Test-Path -LiteralPath $manifest)) {
        throw "FFmpeg dep manifest not found at $manifest - restore driver_cardboardplusplus\lib\ffmpeg\deps.json"
    }
    $json = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
    if (-not $json.dlls -or $json.dlls.Count -eq 0) {
        throw "FFmpeg dep manifest at $manifest lists no dlls - fix deps.json"
    }
    return $json
}

# Pinned runtime DLL file names, e.g. avcodec-62.dll
function Get-FfmpegDepDlls([string]$FfmpegDir) {
    return @( (Get-FfmpegDepsManifest $FfmpegDir).dlls )
}

# Fail fast when the vendored tree doesn't match the pinned version:
# each pinned DLL needs its link-time .lib + major-tied .def under lib/
# and its runtime DLL under bin/. Runs after Ensure-FfmpegDeps (compile,
# install) so a version skew is never a silent dead driver.
function Assert-FfmpegDeps([string]$FfmpegDir) {
    $manifest = Get-FfmpegDepsManifest $FfmpegDir
    $libDir = Join-Path $FfmpegDir "lib"
    $binDir = Join-Path $FfmpegDir "bin"
    $bad = @()
    foreach ($dll in $manifest.dlls) {
        $base = [System.IO.Path]::GetFileNameWithoutExtension($dll)  # avcodec-62
        $libName = ($base -replace '-\d+$', '')                        # avcodec
        if (-not (Test-Path -LiteralPath (Join-Path $libDir "$libName.lib"))) {
            $bad += "lib\$libName.lib"
        }
        if (-not (Test-Path -LiteralPath (Join-Path $libDir "$base.def"))) {
            $bad += "lib\$base.def"
        }
        if (-not (Test-Path -LiteralPath (Join-Path $binDir $dll))) {
            $bad += "bin\$dll"
        }
    }
    if ($bad.Count -gt 0) {
        throw ("FFmpeg {0} files missing under {1} ({2}) - link-time files stay vendored, runtime DLLs auto-fetch via Ensure-FfmpegDeps; if the pin itself moved, bump dlls + bin_zip_url in deps.json together" -f $manifest.ffmpeg_version, $FfmpegDir, ($bad -join ", "))
    }
    Write-Host ("FFmpeg {0} deps OK: {1}" -f $manifest.ffmpeg_version, ($manifest.dlls -join ", "))
}

# Download the pinned shared build from GitHub and extract just the runtime
# DLLs into bin/. No-op when all pinned DLLs are already present (bin/ is
# git-ignored, so this runs once per fresh clone). Ends with Assert, so a
# renamed/removed upstream asset fails loudly instead of half-installing.
function Ensure-FfmpegDeps([string]$FfmpegDir) {
    $manifest = Get-FfmpegDepsManifest $FfmpegDir
    $binDir = Join-Path $FfmpegDir "bin"
    $need = @($manifest.dlls | Where-Object { -not (Test-Path -LiteralPath (Join-Path $binDir $_)) })
    if ($need.Count -eq 0) {
        Assert-FfmpegDeps $FfmpegDir
        return
    }
    if (-not $manifest.bin_zip_url) {
        throw "deps.json has no bin_zip_url - add the BtbN shared-build zip URL for FFmpeg $($manifest.ffmpeg_version)"
    }
    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("ffmpeg-dl-" + [System.IO.Path]::GetRandomFileName())
    New-Item -ItemType Directory -Path $tmp -Force | Out-Null
    try {
        $zip = Join-Path $tmp "ffmpeg-shared.zip"
        Write-Host ("Fetching FFmpeg {0} runtime ({1}) ..." -f $manifest.ffmpeg_version, $manifest.bin_zip_url) -ForegroundColor Cyan
        # curl resumes partial downloads (-C -) and retries drops; the GitHub
        # CDN can be slow, and re-downloading ~90 MB from scratch each time
        # is what made naive Invoke-WebRequest runs die here before.
        $curl = Get-Command curl.exe -ErrorAction SilentlyContinue
        if ($curl) {
            & $curl.Source -fSL --retry 3 --retry-delay 5 -C - -o $zip $manifest.bin_zip_url
            if ($LASTEXITCODE -ne 0) { throw "curl download failed (exit $LASTEXITCODE)" }
        } else {
            $ProgressPreference = "SilentlyContinue"
            # Throws on network/HTTP errors ($ErrorActionPreference = "Stop").
            Invoke-WebRequest -Uri $manifest.bin_zip_url -OutFile $zip -UseBasicParsing
        }
        Expand-Archive -LiteralPath $zip -DestinationPath (Join-Path $tmp "x") -Force
        New-Item -ItemType Directory -Path $binDir -Force | Out-Null
        foreach ($dll in $manifest.dlls) {
            $found = Get-ChildItem -Path (Join-Path $tmp "x") -Filter $dll -File -Recurse -ErrorAction SilentlyContinue |
                Select-Object -First 1
            if (-not $found) { throw "zip has no $dll - upstream renamed the asset; bump bin_zip_url in deps.json" }
            Copy-Item -LiteralPath $found.FullName -Destination (Join-Path $binDir $dll) -Force
        }
        Write-Host ("Fetched: {0}" -f ($manifest.dlls -join ", ")) -ForegroundColor Green
    } finally {
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
    Assert-FfmpegDeps $FfmpegDir
}
