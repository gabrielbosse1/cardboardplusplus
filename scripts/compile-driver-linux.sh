#!/usr/bin/env bash
# Build the Linux SteamVR driver. Produces build/driver_cardboardplusplus.so;
# copy it to $HOME/.local/share/SteamVR/drivers/cardboardplusplus/bin/linux64/.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${here}/driver_cardboardplusplus/build"

# SteamVR ships openvr_api.so; export OPENVR_DIR if it is not on the default path.
# Check multiple possible SteamVR install locations (native Steam, Flatpak, etc.)
if [ -z "${OPENVR_DIR:-}" ]; then
    for candidate in \
        "${HOME}/.steam/steam/steamapps/common/SteamVR" \
        "${HOME}/.local/share/Steam/steamapps/common/SteamVR" \
        "${HOME}/.var/app/com.valvesoftware.Steam/.steam/steam/steamapps/common/SteamVR" \
        "${HOME}/.var/app/com.valvesoftware.Steam/steam/steamapps/common/SteamVR" \
        "${HOME}/snap/steam/common/SteamVR"; do
        if [ -f "${candidate}/bin/linux64/openvr_api.so" ]; then
            export OPENVR_DIR="$candidate"
            break
        fi
    done
fi
if [ -z "${OPENVR_DIR:-}" ]; then
    echo "OPENVR_DIR not set and no SteamVR install found in the default paths." >&2
    echo "SteamVR paths checked:" >&2
    echo "  ${HOME}/.steam/steam/steamapps/common/SteamVR" >&2
    echo "  ${HOME}/.local/share/Steam/steamapps/common/SteamVR" >&2
    echo "  ${HOME}/.var/app/com.valvesoftware.Steam/.steam/steam/steamapps/common/SteamVR" >&2
    echo "  ${HOME}/.var/app/com.valvesoftware.Steam/steam/steamapps/common/SteamVR" >&2
    echo "  ${HOME}/snap/steam/common/SteamVR" >&2
    echo "" >&2
    echo "Set OPENVR_DIR to your SteamVR install directory, e.g.:" >&2
    echo "  export OPENVR_DIR=\$HOME/.local/share/Steam/steamapps/common/SteamVR" >&2
    exit 1
fi

# Check that dxc is available (required for SPIR-V shader compilation).
if ! command -v dxc >/dev/null 2>&1; then
    echo "dxc (DirectXShaderCompiler) not found on PATH." >&2
    echo "Install it:" >&2
    echo "  curl -fsSL -o /tmp/dxc.tar.gz https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/linux_dxc_2026_09_28.x86_x64.tar.gz" >&2
    echo "  sudo mkdir -p /opt/dxc && sudo tar -xzf /tmp/dxc.tar.gz -C /opt/dxc" >&2
    echo "  sudo ln -sf \$(find /opt/dxc -name dxc -type f | head -1) /usr/local/bin/dxc" >&2
    exit 1
fi

cmake -S "${here}/driver_cardboardplusplus" -B "$build" -DCMAKE_BUILD_TYPE=Release \
    -DCBPP_OPENVR_DIR="$OPENVR_DIR/bin/linux64" "$@"
cmake --build "$build" --parallel "$(nproc)"

echo "Built: $build/libdriver_cardboardplusplus.so"
echo "Install: bash scripts/install-driver-linux.sh"