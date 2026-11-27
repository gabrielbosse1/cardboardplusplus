#!/usr/bin/env bash
# Install the bridge control plane on Linux: build/install the binary, stage the
# MediaPipe sidecar files, and refresh the launcher entry.
set -euo pipefail
scripts_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
here="$(cd "${scripts_dir}/.." && pwd)"
# shellcheck source=lib/linux-common.sh
source "${scripts_dir}/lib/linux-common.sh"

no_build=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --no-build) no_build=1 ;;
        *) cbpp_fail "unknown option: $1" ;;
    esac
    shift
done

bin_dir="$(cbpp_bin_dir)"
data_dir="$(cbpp_data_dir)"
mkdir -p "$bin_dir" "$data_dir/models"

if [ "$no_build" -eq 0 ]; then
    if ! cbpp_has_cmd cargo; then
        cbpp_warn "cargo not on PATH; install Rust (https://rustup.rs) or use --no-build with a prebuilt binary"
    else
        cbpp_log "building bridge (cargo build --release)…"
        cargo build --release --manifest-path "${here}/bridge/Cargo.toml"
    fi
fi

src=""
if [ -n "${CBPP_BRIDGE_BIN:-}" ] && [ -x "$CBPP_BRIDGE_BIN" ]; then
    src="$CBPP_BRIDGE_BIN"
fi
if [ -z "$src" ]; then
    for candidate in \
        "${here}/bridge/target/release/cardboard-bridge-svc" \
        "${here}/bridge/target/debug/cardboard-bridge-svc"; do
        [ -x "$candidate" ] && { src="$candidate"; break; }
    done
fi
if [ -z "$src" ]; then
    cbpp_fail "bridge binary not found; build it first: cargo build --release --manifest-path bridge/Cargo.toml"
fi
cp "$src" "${bin_dir}/cardboard-bridge-svc"
chmod +x "${bin_dir}/cardboard-bridge-svc"
cbpp_log "installed ${bin_dir}/cardboard-bridge-svc"

cp "${here}/bridge/crates/cardboard-bridge/mediapipe_server.py" "${data_dir}/mediapipe_server.py"
cp "${here}/bridge/crates/cardboard-bridge/models/hand_landmarker.task" "${data_dir}/models/hand_landmarker.task"
cbpp_log "staged MediaPipe sidecar + model in ${data_dir}"

# Stage a self-contained driver bundle so the in-app Install button works
# without a checkout (curl-installed bridge) and without these scripts.
driver_so="${CBPP_DRIVER_SO:-${here}/driver_cardboardplusplus/build/libdriver_cardboardplusplus.so}"
if [ -f "$driver_so" ]; then
    mkdir -p "${data_dir}/driver/resources"
    cp "$driver_so" "${data_dir}/driver/driver_cardboardplusplus.so"
    cp "${here}/driver_cardboardplusplus/resources/driver.vrdrivermanifest" "${data_dir}/driver/driver.vrdrivermanifest"
    cp -r "${here}/driver_cardboardplusplus/resources/." "${data_dir}/driver/resources/"
    cbpp_log "staged driver bundle in ${data_dir}/driver"
else
    cbpp_warn "driver bundle not staged; install the driver with scripts/install-linux.sh --driver to enable the in-app Install button"
fi

cbpp_install_desktop_entry "$here"

if cbpp_has_cmd ffmpeg; then
    cbpp_log "ffmpeg found: $(ffmpeg -version 2>/dev/null | head -1)"
else
    cbpp_warn "ffmpeg not found — preview stays dark. Install it with --deps or your package manager."
fi

echo
cbpp_log "bridge installed. Launch from the applications menu, or: ${bin_dir}/cardboard-bridge-svc"
