#!/usr/bin/env bash
# Launches VRChat in SteamVR mode. VRChat falls back to desktop when OpenVR is
# not initialized at startup, so this waits for SteamVR to be Ready first.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${here}/lib/linux-common.sh"

log="$HOME/.local/share/Steam/logs/vrmonitor.txt"
ready() {
    pgrep -x vrserver >/dev/null 2>&1 && tail -n 80 "$log" 2>/dev/null | grep -q 'SteamVRSystemState_Ready'
}

cbpp_enable_driver_in_vrsettings
if ! ready; then
    cbpp_log "starting SteamVR"
    "$HOME/.steam/steam/steamapps/common/SteamVR/bin/vrmonitor.sh" >/dev/null 2>&1 &
    cbpp_log "waiting for SteamVR Ready"
    for _ in $(seq 1 60); do
        ready && break
        sleep 1
    done
fi
if ! ready; then
    cbpp_fail "SteamVR did not reach Ready; check $log"
fi

if ! grep -q 'Loaded server driver cardboardplusplus' \
        "$HOME/.local/share/Steam/logs/vrserver.txt" 2>/dev/null; then
    cbpp_warn "our driver is not loaded; SteamVR may need a restart"
fi

cbpp_log "launching VRChat (-vrmode steamvr)"
"$HOME/.local/share/Steam/steam.sh" -applaunch 438100
