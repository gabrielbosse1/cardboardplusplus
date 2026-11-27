#!/usr/bin/env bash
# Steam launch-option wrapper: makes sure SteamVR is Ready before VRChat starts.
# Used as: /path/to/steamvr-vrchat-launch.sh %command%
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${here}/lib/linux-common.sh"

log="$HOME/.local/share/Steam/logs/vrmonitor.txt"
ready() {
    pgrep -x vrserver >/dev/null 2>&1 && tail -n 80 "$log" 2>/dev/null | grep -q 'SteamVRSystemState_Ready'
}
cbpp_enable_driver_in_vrsettings
if ! ready; then
    "$HOME/.steam/steam/steamapps/common/SteamVR/bin/vrmonitor.sh" >/dev/null 2>&1 &
    for _ in $(seq 1 60); do
        ready && break
        sleep 1
    done
fi
if ! ready; then
    cbpp_warn "SteamVR did not reach Ready; VRChat may fall back to desktop"
fi
exec "$@"
