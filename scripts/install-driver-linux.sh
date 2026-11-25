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
#   ~/.local/share/icons/hicolor/<size>/apps/cardboardplusplus.png
#
# The driver links against system FFmpeg (libavcodec/libavformat/libavutil/
# libswscale via pkg-config), so no FFmpeg shared libs are shipped — install
# the dev packages for your distro before building (see the guide).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${here}/driver_cardboardplusplus/build"
so="${build}/libdriver_cardboardplusplus.so"

[ -f "$so" ] || { echo "ERROR: Driver not built." >&2; echo "  Build it first: bash scripts/compile-driver-linux.sh" >&2; exit 1; }

# ── Detect desktop environment ────────────────────────────────────────────────
# If there's no DE, installing .desktop and icon files is harmless but pointless.
HAS_DE=0
if [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; then
    HAS_DE=1
elif command -v xdg-open >/dev/null 2>&1; then
    HAS_DE=1
fi

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
icon_base="${HOME}/.local/share/icons/hicolor"
mkdir -p "$bin_dir" "$apps_dir"

bridge_installed=0
if [ -n "$bridge_bin" ]; then
    cp "$bridge_bin" "${bin_dir}/cardboard-bridge-svc"
    chmod +x "${bin_dir}/cardboard-bridge-svc"
    echo "Installed: ${bin_dir}/cardboard-bridge-svc"
    bridge_installed=1
fi

# Desktop entry (launcher icon + app menu entry) — from the app/ directory,
# not resources/ (resources/ holds SteamVR driver files only).
desktop_src="${here}/driver_cardboardplusplus/app/cardboardplusplus.desktop"
[ -f "$desktop_src" ] || { echo "Missing desktop entry: $desktop_src" >&2; exit 1; }

if [ "$HAS_DE" -eq 1 ]; then
    cp "$desktop_src" "${apps_dir}/cardboardplusplus.desktop"
    echo "Installed: ${apps_dir}/cardboardplusplus.desktop"
else
    echo "Note: No desktop environment detected (DISPLAY/WAYLAND_DISPLAY not set)." >&2
    echo "      Desktop entry not installed — this is expected on headless systems." >&2
fi

# App icons for the launcher — install all available sizes so every DE can
# pick the right one (some request 16x16 for task bars, 256x256 for app menus).
icon_dir="${here}/driver_cardboardplusplus/app/icons"
if [ -d "$icon_dir" ]; then
    icon_count=0
    while IFS= read -r -d '' f; do
        # Extract the size from the filename: cardboardplusplus-<WxH>.png
        fname="$(basename "$f")"
        size_part="${fname#cardboardplusplus-}"
        size_part="${size_part%.png}"
        # Only handle <WxH> pattern; skip the legacy single-size file
        if [[ "$size_part" =~ ^([0-9]+)x([0-9]+)$ ]]; then
            size_dir="${icon_base}/${size_part}/apps"
            mkdir -p "$size_dir"
            cp "$f" "${size_dir}/cardboardplusplus.png"
            echo "Installed: ${size_dir}/cardboardplusplus.png (${size_part})"
            icon_count=$((icon_count + 1))
        fi
    done < <(find "$icon_dir" -name 'cardboardplusplus-*x*.png' -print0)

    if [ "$icon_count" -eq 0 ]; then
        # Fallback: install the legacy 128x128 icon if no sized icons found
        legacy_icon="${icon_dir}/cardboardplusplus.png"
        if [ -f "$legacy_icon" ]; then
            size_dir="${icon_base}/128x128/apps"
            mkdir -p "$size_dir"
            cp "$legacy_icon" "${size_dir}/cardboardplusplus.png"
            echo "Installed: ${size_dir}/cardboardplusplus.png (legacy 128x128)"
        fi
    fi

    # Refresh the icon cache so the desktop picks up the new icons immediately.
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache "$icon_base" 2>/dev/null || true
        echo "Refreshed icon cache"
    fi
fi

# Refresh the desktop database so the launcher picks up the new app entry.
if command -v update-desktop-database >/dev/null 2>&1 && [ -d "$apps_dir" ]; then
    update-desktop-database "$apps_dir" 2>/dev/null || true
    echo "Refreshed desktop database"
fi

# ── Summary & post-install guidance ───────────────────────────────────────────

echo
echo "Driver installed to ${dest_root}"

if [ "$bridge_installed" -eq 1 ]; then
    echo "Bridge app installed to ${bin_dir}/cardboard-bridge-svc"
else
    echo "Bridge binary NOT found in bridge/target/ — the desktop entry will not"
    echo "launch until you build it:"
    echo "  1. Install Rust:  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh"
    echo "  2. Build the bridge:  cd bridge && cargo build --release"
    echo "  3. Re-run this installer"
fi

echo "Restart SteamVR to load the driver; run 'cardboard-bridge-svc' to launch the bridge."
