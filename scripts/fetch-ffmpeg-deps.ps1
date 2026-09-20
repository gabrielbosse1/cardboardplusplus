# Thin entry so the bridge (Rust) can trigger the same FFmpeg fetch the
# build/install scripts use. No args; exit code + stdout are the contract
# (the bridge surfaces both on failure). Throws when the fetch or assert fails.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot\ffmpeg-deps.ps1"
Ensure-FfmpegDeps "$root\driver_cardboardplusplus\lib\ffmpeg"
