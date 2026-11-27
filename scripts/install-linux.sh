#!/usr/bin/env bash
# Universal Cardboard++ installer for Linux. Detects the distro/package manager
# and runs the same steps as the Windows installers, per component:
#   --deps       install build + runtime packages (default; needs sudo/doas)
#   --no-deps    skip the distro package step
#   --driver     SteamVR driver (.so, manifest, resources, registration)
#   --bridge     bridge binary + launcher/desktop entry
#   --mediapipe  MediaPipe sidecar in a per-user Python venv
#   --steamvr    register/enable the driver in SteamVR
#   --start      launch SteamVR after installing
#   --uninstall  remove the per-user installs
# With no arguments, everything except --deps and --start runs.
set -euo pipefail
scripts_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
here="$(cd "${scripts_dir}/.." && pwd)"
# shellcheck source=lib/linux-common.sh
source "${scripts_dir}/lib/linux-common.sh"

do_deps=1 do_driver=0 do_bridge=0 do_mediapipe=0 do_steamvr=0 do_start=0 do_uninstall=0
explicit=0
while [ "$#" -gt 0 ]; do
    explicit=1
    case "$1" in
        --all) do_driver=1 do_bridge=1 do_mediapipe=1 do_steamvr=1 ;;
        --deps) do_deps=1 ;;
        --no-deps) do_deps=0 ;;
        --driver) do_driver=1 ;;
        --bridge) do_bridge=1 ;;
        --mediapipe) do_mediapipe=1 ;;
        --desktop) do_bridge=1 ;;
        --steamvr) do_steamvr=1 ;;
        --start) do_start=1 ;;
        --uninstall) do_uninstall=1 ;;
        --no-build) export CBPP_NO_BUILD=1 ;;
        -h|--help)
            sed -n '2,13p' "${BASH_SOURCE[0]}"
            exit 0 ;;
        *) cbpp_fail "unknown option: $1 (try --help)" ;;
    esac
    shift
done
if [ "$explicit" -eq 0 ]; then
    do_driver=1 do_bridge=1 do_mediapipe=1 do_steamvr=1
fi
if [ "$do_uninstall" -eq 1 ]; then
    exec bash "${here}/scripts/uninstall-linux.sh"
fi

fields="$(cbpp_distro_fields)"
cbpp_log "distro: ${fields%%;*} (package manager: $(printf '%s' "$fields" | cut -d';' -f2))"

if [ "$do_deps" -eq 1 ]; then
    pm="$(printf '%s' "$fields" | cut -d';' -f2)"
    case "$pm" in
        apt)
            cbpp_install_packages build-essential cmake pkg-config libavcodec-dev libavformat-dev \
                libavutil-dev libswscale-dev libvulkan-dev libx11-dev ffmpeg python3 python3-venv \
                python3-pip curl ca-certificates patchelf ;;
        dnf|yum)
            cbpp_install_packages gcc-c++ cmake pkgconf-pkg-config ffmpeg-devel vulkan-devel \
                libX11-devel python3 python3-pip python3-virtualenv curl ca-certificates patchelf ;;
        pacman)
            cbpp_install_packages base-devel cmake pkgconf ffmpeg vulkan-headers libx11 \
                python python-pip curl ca-certificates patchelf ;;
        apk)
            cbpp_install_packages build-base cmake pkgconf ffmpeg-dev vulkan-headers libx11-dev \
                python3 py3-pip curl ca-certificates patchelf ;;
        zypper)
            cbpp_install_packages gcc-c++ cmake pkg-config ffmpeg-devel vulkan-devel libX11-devel \
                python3 python3-pip curl ca-certificates patchelf ;;
        *) cbpp_warn "unsupported package manager ($pm); install build tools, FFmpeg/Vulkan/X11 dev libs and python3 manually" ;;
    esac
fi

# MediaPipe first: it creates the venv that supplies patchelf for the driver's
# staged FFmpeg libraries.
[ "$do_mediapipe" -eq 1 ] && bash "${here}/scripts/install-mediapipe-linux.sh"
[ "$do_driver" -eq 1 ] && bash "${here}/scripts/install-driver-linux.sh"
if [ "$do_bridge" -eq 1 ]; then
    if [ "${CBPP_NO_BUILD:-0}" = "1" ]; then
        bash "${here}/scripts/install-bridge-linux.sh" --no-build
    else
        bash "${here}/scripts/install-bridge-linux.sh"
    fi
fi
[ "$do_steamvr" -eq 1 ] && bash "${here}/scripts/install-steamvr-linux.sh"
if [ "$do_start" -eq 1 ]; then
    cbpp_start_steamvr || cbpp_warn "could not start SteamVR automatically"
fi

echo
cbpp_log "install complete. Launch Cardboard++ from your applications menu."
