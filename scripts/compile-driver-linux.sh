#!/usr/bin/env bash
# Build the Linux SteamVR driver. Produces build/libdriver_cardboardplusplus.so.
# Locates SteamVR's openvr_api.so across native/Flatpak/Snap layouts and
# bootstraps dxc into the per-user data dir when it is not on PATH.
set -euo pipefail
scripts_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
here="$(cd "${scripts_dir}/.." && pwd)"
# shellcheck source=lib/linux-common.sh
source "${scripts_dir}/lib/linux-common.sh"

build="${here}/driver_cardboardplusplus/build"
openvr="$(cbpp_find_openvr_dir 2>/dev/null || true)"
[ -n "$openvr" ] || cbpp_fail "SteamVR not found. Set OPENVR_DIR to a directory containing bin/linux64/openvr_api.so"

dxc=""
if cbpp_has_cmd dxc; then
    dxc="$(command -v dxc)"
else
    data_dir="$(cbpp_data_dir)"
    dxc="${data_dir}/dxc/dxc"
    if [ ! -x "$dxc" ]; then
        cbpp_log "dxc not found — downloading DirectXShaderCompiler"
        url="https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/linux_dxc_2026_09_28.x86_x64.tar.gz"
        archive="$(mktemp)"
        cbpp_download "$url" "$archive"
        mkdir -p "${data_dir}/dxc"
        tar -xzf "$archive" -C "${data_dir}/dxc"
        rm -f "$archive"
        found="$(find "${data_dir}/dxc" -type f -name dxc | head -1)"
        [ -n "$found" ] || cbpp_fail "dxc archive did not contain a dxc binary"
        dxc="$found"
        chmod +x "$dxc"
    fi
fi
cbpp_log "dxc: $dxc"
cbpp_log "openvr: $openvr"

cmake -S "${here}/driver_cardboardplusplus" -B "$build" -DCMAKE_BUILD_TYPE=Release \
    -DCBPP_OPENVR_DIR="${openvr}/bin/linux64" -DCBPP_DXC_EXE="$dxc" "$@"
cmake --build "$build" --parallel "$(nproc 2>/dev/null || echo 2)"

echo "Built: $build/libdriver_cardboardplusplus.so"
echo "Install: bash scripts/install-driver-linux.sh (or scripts/install-linux.sh --all)"
