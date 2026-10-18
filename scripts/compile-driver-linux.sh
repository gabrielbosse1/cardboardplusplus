#!/usr/bin/env bash
# Build the Linux SteamVR driver. Produces build/driver_cardboardplusplus.so;
# copy it to $HOME/.local/share/SteamVR/drivers/cardboardplusplus/bin/linux64/.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${here}/driver_cardboardplusplus/build"

# SteamVR ships openvr_api.so; export OPENVR_DIR if it is not on the default path.
if [ -z "${OPENVR_DIR:-}" ]; then
    for candidate in \
        "$HOME/.steam/steam/steamapps/common/SteamVR" \
        "$HOME/.local/share/Steam/steamapps/common/SteamVR"; do
        if [ -f "$candidate/bin/linux64/openvr_api.so" ]; then
            export OPENVR_DIR="$candidate"
            break
        fi
    done
fi
if [ -z "${OPENVR_DIR:-}" ]; then
    echo "OPENVR_DIR not set and no SteamVR install found in the default paths." >&2
    exit 1
fi

cmake -S "${here}/driver_cardboardplusplus" -B "$build" -DCMAKE_BUILD_TYPE=Release \
    -DCBPP_OPENVR_DIR="$OPENVR_DIR/bin/linux64" "$@"
cmake --build "$build" --parallel "$(nproc)"

echo "Built: $build/libdriver_cardboardplusplus.so"
echo "Install: mkdir -p \$HOME/.local/share/SteamVR/drivers/cardboardplusplus/bin/linux64/ &&"
echo "         cp $build/libdriver_cardboardplusplus.so \$HOME/.local/share/SteamVR/drivers/cardboardplusplus/bin/linux64/"
echo "Enable:   \$OPENVR_DIR/bin/linux64/vrserver.sh -vrdriver cardboardplusplus"