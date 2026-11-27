#!/usr/bin/env bash
# Install the Linux SteamVR driver (and the launcher entry) with the same steps
# as scripts/install-driver.ps1: stop SteamVR, back up, copy .so + manifest +
# resources, register the driver, enable it, refresh the app launcher.
set -euo pipefail
scripts_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
here="$(cd "${scripts_dir}/.." && pwd)"
# shellcheck source=lib/linux-common.sh
source "${scripts_dir}/lib/linux-common.sh"

build="${here}/driver_cardboardplusplus/build"
so="${CBPP_DRIVER_SO:-${build}/libdriver_cardboardplusplus.so}"
[ -f "$so" ] || cbpp_fail "driver .so not found; build it with scripts/compile-driver-linux.sh or set CBPP_DRIVER_SO to a downloaded .so"

if [ "${CBPP_SKIP_STOP:-0}" != "1" ]; then
    cbpp_stop_steamvr
fi

registered="$(cbpp_registered_driver_dir || true)"
case "$registered" in
    /*/drivers/cardboardplusplus) dest_root="$registered" ;;
    *) dest_root="$(cbpp_default_driver_dir)" ;;
esac
dest_bin="${dest_root}/bin/linux64"
dest_res="${dest_root}/resources"
mkdir -p "$dest_bin" "$dest_res"

if [ -f "${dest_bin}/driver_cardboardplusplus.so" ]; then
    stamp="$(date +%Y%m%d-%H%M%S)"
    cp "${dest_bin}/driver_cardboardplusplus.so" \
       "${dest_bin}/driver_cardboardplusplus.so.bak.${stamp}"
    cbpp_log "existing driver backed up as .bak.${stamp}"
fi
cp "$so" "${dest_bin}/driver_cardboardplusplus.so"
cbpp_log "installed ${dest_bin}/driver_cardboardplusplus.so"

# Steam starts vrserver in its own runtime. DT_RUNPATH is not inherited by
# dependencies, so every library in the closure must live beside the driver
# and carry its own $ORIGIN RUNPATH. Core libc/loader libs stay with Steam.
patcher="$(cbpp_patchelf || true)"
declare -A staged=()
stage_closure() {
    local target="$1" libname libpath
    while read -r libname libpath; do
        [ -n "$libpath" ] || continue
        case "$libpath" in /*) ;; *) continue ;; esac
        case "$libname" in
            libc.so.6|libm.so.6|libpthread.so.0|libdl.so.2|librt.so.1|libgcc_s.so.1|\
            ld-linux*|linux-vdso*|libopenvr_api.so|openvr_api.so|libvulkan.so.1|libX11.so.6) continue ;;
        esac
        [ -z "${staged[$libname]:-}" ] || continue
        staged[$libname]=1
        cp -L "$libpath" "${dest_bin}/${libname}" || continue
        if [ -n "$patcher" ]; then
            "$patcher" --set-rpath '$ORIGIN:/run/host/usr/lib:/usr/lib:/usr/lib64' \
                "${dest_bin}/${libname}" 2>/dev/null || true
        fi
        stage_closure "${dest_bin}/${libname}"
    done < <(ldd "$target" 2>/dev/null | awk 'NF >= 3 && $2 == "=>" { print $1, $3 }')
}
if cbpp_has_cmd ldd; then
    stage_closure "$so"
fi
cbpp_log "staged ${#staged[@]} runtime library(ies) beside the driver"

manifest_src="${here}/driver_cardboardplusplus/resources/driver.vrdrivermanifest"
[ -f "$manifest_src" ] || cbpp_fail "missing manifest: $manifest_src"
cp "$manifest_src" "${dest_root}/driver.vrdrivermanifest"

res_count=0
while IFS= read -r -d '' f; do
    rel="${f#${here}/driver_cardboardplusplus/resources/}"
    target="${dest_res}/${rel}"
    mkdir -p "$(dirname "$target")"
    cp "$f" "$target"
    res_count=$((res_count + 1))
done < <(find "${here}/driver_cardboardplusplus/resources" -type f -print0)
cbpp_log "installed manifest and ${res_count} resource file(s) into ${dest_root}"

cbpp_register_driver "$dest_root"
cbpp_enable_driver_in_vrsettings
cbpp_install_desktop_entry "$here"

echo
cbpp_log "driver installed to ${dest_root}"
cbpp_log "restart SteamVR to load it: bash scripts/install-steamvr-linux.sh --start"
