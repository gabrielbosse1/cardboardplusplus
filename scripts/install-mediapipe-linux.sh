#!/usr/bin/env bash
# Install the MediaPipe hand-tracking sidecar on Linux into a per-user venv.
# Works on distros that mark the system Python as externally managed.
set -euo pipefail
scripts_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
here="$(cd "${scripts_dir}/.." && pwd)"
# shellcheck source=lib/linux-common.sh
source "${scripts_dir}/lib/linux-common.sh"

data_dir="$(cbpp_data_dir)"
venv="${data_dir}/mediapipe-venv"
requirements="${here}/bridge/crates/cardboard-bridge/requirements.txt"
system=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --system) system=1 ;;
        *) cbpp_fail "unknown option: $1" ;;
    esac
    shift
done

py="$(command -v python3 || true)"
[ -n "$py" ] || cbpp_fail "python3 not found; install it with --deps or from your distro"

mkdir -p "${data_dir}/models"
cp "${here}/bridge/crates/cardboard-bridge/mediapipe_server.py" "${data_dir}/mediapipe_server.py"
cp "${here}/bridge/crates/cardboard-bridge/models/hand_landmarker.task" "${data_dir}/models/hand_landmarker.task"

if [ "$system" -eq 0 ]; then
    if [ ! -x "${venv}/bin/python" ]; then
        cbpp_log "creating venv at ${venv}"
        if ! "$py" -m venv "$venv" >/dev/null 2>&1; then
            cbpp_warn "could not create venv; install python3-venv (apt) / python3-virtualenv (dnf) / python-virtualenv (pacman), or rerun with --system"
            exit 1
        fi
    fi
    "$venv/bin/python" -m pip install --quiet --upgrade pip
    "$venv/bin/python" -m pip install --quiet -r "$requirements"
    cbpp_log "installed mediapipe, opencv-python and numpy into the venv"
else
    cbpp_log "installing sidecar requirements into the user site-packages"
    "$py" -m pip install --quiet --user -r "$requirements"
fi

if [ "$system" -eq 0 ]; then
    "$venv/bin/python" - <<'PY'
import cv2, mediapipe, numpy
print("mediapipe sidecar deps OK")
PY
else
    "$py" - <<'PY'
import cv2, mediapipe, numpy
print("mediapipe sidecar deps OK")
PY
fi
cbpp_log "sidecar ready: $(cbpp_venv_python)"
