#!/usr/bin/env bash
# SteamVR integration for Linux: locate SteamVR, register our driver, clear the
# safe-mode block, and optionally start SteamVR.
set -euo pipefail
scripts_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
here="$(cd "${scripts_dir}/.." && pwd)"
# shellcheck source=lib/linux-common.sh
source "${scripts_dir}/lib/linux-common.sh"

start=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --start) start=1 ;;
        *) cbpp_fail "unknown option: $1" ;;
    esac
    shift
done

openvr="$(cbpp_find_openvr_dir 2>/dev/null || true)"
if [ -z "$openvr" ]; then
    cbpp_warn "SteamVR not found. Install Steam, then Steam → Library → Tools → SteamVR."
    cbpp_warn "Package hints: 'steam' (apt/dnf/pacman), Flatpak 'com.valvesoftware.Steam', or Snap 'steam'."
    exit 1
fi
cbpp_log "SteamVR runtime: $openvr"

registered="$(cbpp_registered_driver_dir || true)"
case "$registered" in
    /*/drivers/cardboardplusplus) dest_root="$registered" ;;
    *) dest_root="$(cbpp_default_driver_dir)" ;;
esac
if [ ! -f "${dest_root}/driver.vrdrivermanifest" ]; then
    cbpp_warn "driver not installed at ${dest_root}; run scripts/install-driver-linux.sh first"
fi
cbpp_register_driver "$dest_root"
cbpp_enable_driver_in_vrsettings

if [ "$start" -eq 1 ]; then
    cbpp_start_steamvr
else
    cbpp_log "start SteamVR when ready: bash scripts/install-steamvr-linux.sh --start"
fi
