# Installs the Linux driver into the per-user SteamVR driver directory.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${here}/driver_cardboardplusplus/build"
so="${build}/libdriver_cardboardplusplus.so"

[ -f "$so" ] || { echo "Build first: scripts/compile-driver-linux.sh" >&2; exit 1; }

dest="${HOME}/.local/share/SteamVR/drivers/cardboardplusplus/bin/linux64"
mkdir -p "$dest"
cp "$so" "$dest/driver_cardboardplusplus.so"
echo "Installed: $dest/driver_cardboardplusplus.so"