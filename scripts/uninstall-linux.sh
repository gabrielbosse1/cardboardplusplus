#!/usr/bin/env bash
# Removes the per-user Cardboard++ Linux install. System packages installed via
# --deps are intentionally left alone (other software may use them).
set -euo pipefail
scripts_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
here="$(cd "${scripts_dir}/.." && pwd)"
# shellcheck source=lib/linux-common.sh
source "${scripts_dir}/lib/linux-common.sh"

purge_data=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --purge-data) purge_data=1 ;;
        *) cbpp_fail "unknown option: $1" ;;
    esac
    shift
done

cbpp_stop_steamvr

for dir in "$(cbpp_default_driver_dir)" "${HOME}/.local/share/CardboardPlusPlus/drivers/cardboardplusplus"; do
    [ -d "$dir" ] || continue
    rm -rf "$dir"
    cbpp_log "removed driver: $dir"
done

rm -f "$(cbpp_bin_dir)/cardboard-bridge-svc"
rm -f "$(cbpp_applications_dir)/cardboardplusplus.desktop"
find "$(cbpp_icons_dir)" -path '*/apps/cardboardplusplus.png' -delete 2>/dev/null || true
cbpp_log "removed bridge binary, desktop entry and icons"

if cbpp_has_cmd update-desktop-database; then
    update-desktop-database "$(cbpp_applications_dir)" 2>/dev/null || true
fi
if cbpp_has_cmd gtk-update-icon-cache; then
    gtk-update-icon-cache -f -q "$(cbpp_icons_dir)/hicolor" 2>/dev/null || true
fi

if [ "$purge_data" -eq 1 ]; then
    data_dir="$(cbpp_data_dir)"
    rm -rf "${data_dir}/mediapipe-venv" "${data_dir}/setup.done"
    cbpp_log "purged venv and setup wizard marker"
fi

cbpp_log "uninstall complete"
