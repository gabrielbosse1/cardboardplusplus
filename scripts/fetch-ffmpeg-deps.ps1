# fetch-ffmpeg-deps.ps1 — Thin entry point so the bridge (Rust) can trigger
# the same GitHub fetch the build/install scripts use. No args, no output
# contract beyond exit code + stdout (the bridge surfaces both on failure).
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot\ffmpeg-deps.ps1"
# Throws (non-zero exit) when the fetch or the post-fetch assert fails.
Ensure-FfmpegDeps "$root\driver_cardboardplusplus\lib\ffmpeg"
