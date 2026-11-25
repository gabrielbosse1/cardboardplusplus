#!/usr/bin/env bash
# Installs the Linux SteamVR driver and the Cardboard++ bridge app.
#
# Driver layout (mirrors the Windows installer scripts/install-driver.ps1):
#   ~/.local/share/SteamVR/drivers/cardboardplusplus/
#       driver.vrdrivermanifest        (required — SteamVR won't load without it)
#       resources/                     (controller profile, icons, bindings)
#       bin/linux64/driver_cardboardplusplus.so
#
# Bridge app layout (desktop entry + icon so the app shows in the launcher):
#   ~/.local/bin/cardboard-bridge-svc
#   ~/.local/share/applications/cardboardplusplus.desktop
#   ~/.local/share/icons/hicolor/128x128/apps/cardboardplusplus.png
#
# The driver links against system FFmpeg (libavcodec/libavformat/libavutil/
# libswscale via pkg-config), so no FFmpeg shared libs are shipped — install
# them with: sudo apt install libavcodec-dev libavformat-dev libavutil-dev libswscale-dev
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${here}/driver_cardboardplusplus/build"
so="${build}/libdriver_cardboardplusplus.so"

[ -f "$so" ] || { echo "Build first: scripts/compile-driver-linux.sh" >&2; exit 1; }

# ── Driver ────────────────────────────────────────────────────────────────────

# Per-user SteamVR driver directory (SteamVR auto-discovers drivers here).
dest_root="${HOME}/.local/share/SteamVR/drivers/cardboardplusplus"
dest_bin="${dest_root}/bin/linux64"
dest_res="${dest_root}/resources"

# Warn if SteamVR looks running — the .so may be locked while vrserver loads it.
if command -v pgrep >/dev/null 2>&1 && pgrep -x vrserver >/dev/null 2>&1; then
    echo "Warning: vrserver is running. The driver .so may be locked;" >&2
    echo "         quit SteamVR and re-run if the copy fails." >&2
fi

mkdir -p "$dest_bin" "$dest_res"

# Back up the existing .so before replacing it, so a failed install leaves
# the old one in place rather than replacing it with nothing.
if [ -f "${dest_bin}/driver_cardboardplusplus.so" ]; then
    stamp="$(date +%Y%m%d-%H%M%S)"
    cp "${dest_bin}/driver_cardboardplusplus.so" \
       "${dest_bin}/driver_cardboardplusplus.so.bak.${stamp}"
    echo "Existing driver backed up to ${dest_bin}/driver_cardboardplusplus.so.bak.${stamp}"
fi

cp "$so" "${dest_bin}/driver_cardboardplusplus.so"
echo "Installed: ${dest_bin}/driver_cardboardplusplus.so"

# Manifest is required — SteamVR scans the driver root for driver.vrdrivermanifest.
manifest_src="${here}/driver_cardboardplusplus/resources/driver.vrdrivermanifest"
[ -f "$manifest_src" ] || { echo "Missing manifest: $manifest_src" >&2; exit 1; }
cp "$manifest_src" "${dest_root}/driver.vrdrivermanifest"
echo "Installed: ${dest_root}/driver.vrdrivermanifest"

# Copy every file under resources/ (controller profile, icons, bindings).
res_count=0
while IFS= read -r -d '' f; do
    rel="${f#${here}/driver_cardboardplusplus/resources/}"
    target="${dest_res}/${rel}"
    mkdir -p "$(dirname "$target")"
    cp "$f" "$target"
    res_count=$((res_count + 1))
done < <(find "${here}/driver_cardboardplusplus/resources" -type f -print0)
echo "Copied ${res_count} resource file(s) to ${dest_res}"

# ── Bridge app (desktop entry + icon) ─────────────────────────────────────────

# Bridge binary: built by cargo into bridge/target/<profile>/cardboard-bridge-svc.
bridge_bin=""
for candidate in \
    "${here}/bridge/target/release/cardboard-bridge-svc" \
    "${here}/bridge/target/debug/cardboard-bridge-svc"; do
    if [ -x "$candidate" ]; then
        bridge_bin="$candidate"
        break
    fi
done

bin_dir="${HOME}/.local/bin"
apps_dir="${HOME}/.local/share/applications"
icon_dir="${HOME}/.local/share/icons/hicolor/128x128/apps"
mkdir -p "$bin_dir" "$apps_dir" "$icon_dir"

if [ -n "$bridge_bin" ]; then
    cp "$bridge_bin" "${bin_dir}/cardboard-bridge-svc"
    chmod +x "${bin_dir}/cardboard-bridge-svc"
    echo "Installed: ${bin_dir}/cardboard-bridge-svc"
else
    echo "Note: bridge binary not found in bridge/target/ — build it with:" >&2
    echo "  cd bridge && cargo build --release" >&2
    echo "  (desktop entry installed anyway; it will launch once the binary exists)" >&2
fi

# Desktop entry (launcher icon + app menu entry) — from the app/ directory,
# not resources/ (resources/ holds SteamVR driver files only).
desktop_src="${here}/driver_cardboardplusplus/app/cardboardplusplus.desktop"
[ -f "$desktop_src" ] || { echo "Missing desktop entry: $desktop_src" >&2; exit 1; }
cp "$desktop_src" "${apps_dir}/cardboardplusplus.desktop"
echo "Installed: ${apps_dir}/cardboardplusplus.desktop"

# App icon for the launcher.
icon_src="${here}/driver_cardboardplusplus/app/icons/cardboardplusplus.png"
if [ -f "$icon_src" ]; then
    cp "$icon_src" "${icon_dir}/cardboardplusplus.png"
    echo "Installed: ${icon_dir}/cardboardplusplus.png"
else
    echo "Note: app icon not found at $icon_src — launcher will use a default icon" >&2
fi

echo
echo "Driver installed to ${dest_root}"
echo "Bridge app installed to ${bin_dir}/cardboard-bridge-svc"
echo "Restart SteamVR to load the driver; run 'cardboard-bridge-svc' to launch the bridge."
