#!/usr/bin/env bash
# Shared helpers for the Cardboard++ Linux installers. Source-only; no side effects.
# Every path is per-user: installing never needs root except for --deps.

CBPP_NAME="Cardboard++"
CBPP_DRIVER="cardboardplusplus"

cbpp_log()  { printf '[cbpp] %s\n' "$*"; }
cbpp_warn() { printf '[cbpp] WARN: %s\n' "$*" >&2; }
cbpp_fail() { printf '[cbpp] ERROR: %s\n' "$*" >&2; exit 1; }

cbpp_root_dir() {
    cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd
}

cbpp_data_dir() {
    printf '%s' "${XDG_DATA_HOME:-${HOME}/.local/share}/CardboardPlusPlus"
}

cbpp_bin_dir() {
    printf '%s' "${HOME}/.local/bin"
}

cbpp_applications_dir() {
    printf '%s' "${XDG_DATA_HOME:-${HOME}/.local/share}/applications"
}

cbpp_icons_dir() {
    printf '%s' "${XDG_DATA_HOME:-${HOME}/.local/share}/icons"
}

# Prints "id;package_manager;init". Unknown distros fall back to a package
# manager detected by probing the commands, so a minimal container still works.
cbpp_detect_distro() {
    local id="unknown" pm="" init=""
    if [ -r /etc/os-release ]; then
        # shellcheck disable=SC1091
        . /etc/os-release
        id="${ID:-unknown}"
        case "${ID_LIKE:-} ${ID:-}" in
            *debian*|*ubuntu*) pm="apt" ;;
            *fedora*|*rhel*|*centos*) pm="dnf" ;;
            *arch*|*manjaro*) pm="pacman" ;;
            *alpine*) pm="apk" ;;
            *suse*|*opensuse*) pm="zypper" ;;
            *gentoo*) pm="emerge" ;;
            *void*) pm="xbps-install" ;;
        esac
        [ -n "$pm" ] || case "$id" in
            debian|ubuntu|linuxmint|pop|raspbian|kali) pm="apt" ;;
            fedora|rhel|centos|rocky|almalinux) pm="dnf" ;;
            arch|manjaro|endeavouros|cachyos) pm="pacman" ;;
            alpine) pm="apk" ;;
            opensuse*|sles) pm="zypper" ;;
        esac
    fi
    [ -n "$pm" ] || for candidate in apt dnf yum pacman apk zypper xbps-install emerge; do
        if command -v "$candidate" >/dev/null 2>&1; then pm="$candidate"; break; fi
    done
    if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
        init="systemd"
    fi
    printf '%s;%s;%s\n' "$id" "${pm:-none}" "$init"
}

cbpp_pkg_manager() { cbpp_distro_fields | cut -d';' -f2; }
cbpp_distro_fields() { cbpp_detect_distro; }

cbpp_sudo() {
    if [ "$(id -u)" -eq 0 ]; then "$@"
    elif cbpp_has_cmd sudo; then sudo "$@"
    elif cbpp_has_cmd doas; then doas "$@"
    else cbpp_fail "root privileges required (sudo or doas not found)"; fi
}

# Installs distro packages. Called only by the explicit --deps step.
cbpp_install_packages() {
    [ "$#" -gt 0 ] || return 0
    local pm
    pm="$(cbpp_pkg_manager)"
    case "$pm" in
        apt)  cbpp_sudo apt-get update -y && cbpp_sudo apt-get install -y --no-install-recommends "$@" ;;
        dnf|yum) cbpp_sudo "$pm" install -y "$@" ;;
        pacman) cbpp_sudo pacman -Syu --noconfirm --needed "$@" ;;
        apk) cbpp_sudo apk add --no-cache "$@" ;;
        zypper) cbpp_sudo zypper --non-interactive install --auto-agree-with-licenses "$@" ;;
        xbps-install) cbpp_sudo xbps-install -Suy "$@" ;;
        emerge) cbpp_sudo emerge --ask=n --noreplace "$@" ;;
        *) cbpp_fail "no supported package manager found; install these packages manually: $*" ;;
    esac
}

cbpp_has_cmd() { command -v "$1" >/dev/null 2>&1; }

cbpp_download() {
    local url="$1" dest="$2"
    if cbpp_has_cmd curl; then curl -fsSL --retry 3 --max-time 300 -o "$dest" "$url";
    elif cbpp_has_cmd wget; then wget -q -O "$dest" "$url";
    else cbpp_fail "need curl or wget to download $url"; fi
}

# Finds the SteamVR runtime containing bin/linux64/openvr_api.so. Checks the
# native Steam tree, Steam's XDG data dir, Flatpak and Snap layouts.
cbpp_find_openvr_dir() {
    local candidates
    candidates=(
        "${OPENVR_DIR:-}"
        "${HOME}/.steam/steam/steamapps/common/SteamVR"
        "${HOME}/.local/share/Steam/steamapps/common/SteamVR"
        "${HOME}/.var/app/com.valvesoftware.Steam/.steam/steam/steamapps/common/SteamVR"
        "${HOME}/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/SteamVR"
        "${HOME}/snap/steam/common/SteamVR"
        "/opt/SteamVR"
    )
    local dir
    for dir in "${candidates[@]}"; do
        [ -n "$dir" ] || continue
        if [ -f "$dir/bin/linux64/openvr_api.so" ] || [ -f "$dir/bin/linux64/libopenvr_api.so" ]; then
            printf '%s\n' "$dir"
            return 0
        fi
    done
    return 1
}

cbpp_find_vrpathreg() {
    local openvr candidate
    openvr="$(cbpp_find_openvr_dir 2>/dev/null || true)"
    if [ -n "$openvr" ] && [ -x "$openvr/bin/vrpathreg.sh" ]; then
        printf '%s\n' "$openvr/bin/vrpathreg.sh"
        return 0
    fi
    for candidate in \
        "${HOME}/.steam/steam/steamapps/common/SteamVR/bin/vrpathreg.sh" \
        "${HOME}/.local/share/Steam/steamapps/common/SteamVR/bin/vrpathreg.sh" \
        "${HOME}/.var/app/com.valvesoftware.Steam/.steam/steamapps/common/SteamVR/bin/vrpathreg.sh"; do
        if [ -x "$candidate" ]; then printf '%s\n' "$candidate"; return 0; fi
    done
    return 1
}

# Returns the path SteamVR actually loads our driver from, if registered.
cbpp_registered_driver_dir() {
    local reg
    reg="$(cbpp_find_vrpathreg || true)"
    [ -n "$reg" ] || return 0
    "$reg" 2>/dev/null \
        | awk -F' : ' '/^[[:space:]]*cardboardplusplus[[:space:]]*:/ {gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2}' \
        | head -1 || true
}

cbpp_default_driver_dir() {
    printf '%s/drivers/%s\n' "${HOME}/.local/share/SteamVR" "$CBPP_DRIVER"
}

# Directory that holds config/steamvr.vrsettings on this machine.
cbpp_steamvr_config_root() {
    printf '%s' "${HOME}/.local/share/Steam"
}

# Clears the SteamVR safe-mode block for our driver in both config copies.
# Mirrors bridge_core::driver_install::force_driver_enabled.
cbpp_enable_driver_in_vrsettings() {
    local root file
    root="$(cbpp_steamvr_config_root)"
    for file in \
        "${root}/config/steamvr.vrsettings" \
        "${HOME}/.steam/steam/config/steamvr.vrsettings"; do
        for candidate in "$file" "${file}".*; do
        [ -f "$candidate" ] || continue
        python3 - "$candidate" <<'PY' || true
import json, os, sys
path = sys.argv[1]
try:
    with open(path) as f:
        data = json.load(f)
except Exception:
    sys.exit(0)
section = data.get("driver_cardboardplusplus")
if isinstance(section, dict) and "blocked_by_safe_mode" in section:
    section.pop("blocked_by_safe_mode")
    tmp = path + ".cbpp-tmp"
    with open(tmp, "w") as f:
        json.dump(data, f, indent=4)
    os.replace(tmp, path)
PY
        done
    done
}

cbpp_steamvr_processes() {
    pgrep -x vrserver >/dev/null 2>&1 && printf 'vrserver '
    pgrep -x vrmonitor >/dev/null 2>&1 && printf 'vrmonitor '
    pgrep -x vrcompositor >/dev/null 2>&1 && printf 'vrcompositor '
    true
}

# Graceful stop matching scripts/install-driver.ps1: ask SteamVR to quit,
# then TERM, then KILL only as a last resort.
cbpp_stop_steamvr() {
    local procs
    procs="$(cbpp_steamvr_processes)"
    [ -n "$procs" ] || { cbpp_log "SteamVR is not running"; return 0; }
    cbpp_log "stopping SteamVR ($procs)"
    local openvr server
    openvr="$(cbpp_find_openvr_dir 2>/dev/null || true)"
    if [ -n "$openvr" ] && [ -x "$openvr/bin/linux64/vrserver" ]; then
        "$openvr/bin/linux64/vrserver" --quit >/dev/null 2>&1 || true
    fi
    sleep 3
    pkill -x vrserver 2>/dev/null || true
    pkill -x vrmonitor 2>/dev/null || true
    pkill -x vrcompositor 2>/dev/null || true
    pkill -x steamvr_room_se 2>/dev/null || true
    sleep 2
    if [ -n "$(cbpp_steamvr_processes)" ]; then
        pkill -9 -x vrserver 2>/dev/null || true
        pkill -9 -x vrmonitor 2>/dev/null || true
        pkill -9 -x vrcompositor 2>/dev/null || true
    fi
    cbpp_log "SteamVR stopped"
}

cbpp_find_vrmonitor() {
    local openvr candidate
    openvr="$(cbpp_find_openvr_dir 2>/dev/null || true)"
    if [ -n "$openvr" ]; then
        for candidate in "$openvr/bin/vrmonitor.sh" "$openvr/bin/linux64/vrmonitor"; do
            [ -x "$candidate" ] && { printf '%s\n' "$candidate"; return 0; }
        done
    fi
    for candidate in \
        "${HOME}/.steam/steam/steamapps/common/SteamVR/bin/vrmonitor.sh" \
        "${HOME}/.local/share/Steam/steamapps/common/SteamVR/bin/vrmonitor.sh"; do
        [ -x "$candidate" ] && { printf '%s\n' "$candidate"; return 0; }
    done
    return 1
}

cbpp_start_steamvr() {
    local monitor
    monitor="$(cbpp_find_vrmonitor || true)"
    if [ -z "$monitor" ]; then
        cbpp_warn "vrmonitor.sh not found — start SteamVR from the Steam library manually"
        return 1
    fi
    if cbpp_has_cmd setsid; then setsid "$monitor" >/dev/null 2>&1 < /dev/null &
    else "$monitor" >/dev/null 2>&1 < /dev/null & fi
    cbpp_log "launched SteamVR via $monitor"
}

# Registers (or re-registers) the driver so SteamVR scans its directory.
cbpp_register_driver() {
    local dir="$1" reg
    reg="$(cbpp_find_vrpathreg || true)"
    [ -n "$reg" ] || { cbpp_warn "vrpathreg.sh not found; driver may still be auto-discovered"; return 0; }
    local runtime=""
    runtime="$(cbpp_find_openvr_dir 2>/dev/null || true)"
    "$reg" adddriver "$dir" >/dev/null 2>&1 || true
    cbpp_log "registered external driver: $dir (runtime: ${runtime:-unknown})"
}

# Desktop integration, installed regardless of whether a DE is currently
# detected (headless installs become correct when a desktop is started later).
cbpp_install_desktop_entry() {
    local root="$1" apps icons bin source dest size icon icon_count=0
    apps="$(cbpp_applications_dir)"
    icons="$(cbpp_icons_dir)"
    bin="$(cbpp_bin_dir)"
    mkdir -p "$apps" "$icons/hicolor" "$bin"
    source="${root}/driver_cardboardplusplus/app/cardboardplusplus.desktop"
    [ -f "$source" ] || cbpp_fail "missing desktop entry: $source"
    dest="${apps}/cardboardplusplus.desktop"
    # Resolve %h to an absolute path so the entry works under every launcher.
    sed "s|^Exec=.*|Exec=${bin}/cardboard-bridge-svc|; s|^TryExec=.*|TryExec=${bin}/cardboard-bridge-svc|" "$source" > "$dest"
    if ! grep -q '^TryExec=' "$dest"; then
        sed -i "6i TryExec=${bin}/cardboard-bridge-svc" "$dest"
    fi
    chmod 644 "$dest"
    cbpp_log "installed desktop entry: $dest"

    while IFS= read -r -d '' icon; do
        size="$(basename "$icon")"
        size="${size#cardboardplusplus-}"
        size="${size%.png}"
        [[ "$size" =~ ^[0-9]+x[0-9]+$ ]] || continue
        mkdir -p "${icons}/hicolor/${size}/apps"
        cp "$icon" "${icons}/hicolor/${size}/apps/cardboardplusplus.png"
        icon_count=$((icon_count + 1))
    done < <(find "${root}/driver_cardboardplusplus/app/icons" -name 'cardboardplusplus-*x*.png' -print0 2>/dev/null)
    if [ "$icon_count" -eq 0 ] && [ -f "${root}/driver_cardboardplusplus/app/icons/cardboardplusplus.png" ]; then
        mkdir -p "${icons}/hicolor/128x128/apps"
        cp "${root}/driver_cardboardplusplus/app/icons/cardboardplusplus.png" \
           "${icons}/hicolor/128x128/apps/cardboardplusplus.png"
    fi
    cbpp_log "installed $icon_count launcher icon size(s)"

    if cbpp_has_cmd gtk-update-icon-cache; then
        gtk-update-icon-cache -f -q "${icons}/hicolor" 2>/dev/null || true
    fi
    if cbpp_has_cmd update-desktop-database; then
        update-desktop-database "$apps" 2>/dev/null || true
    fi
    if cbpp_has_cmd xdg-desktop-menu; then
        xdg-desktop-menu forceupdate 2>/dev/null || true
    fi
}

cbpp_venv_python() { printf '%s/mediapipe-venv/bin/python\n' "$(cbpp_data_dir)"; }

# ELF RUNPATH patcher. Distros rarely ship patchelf by default, but the PyPI
# wheel is portable and lands in the same venv the MediaPipe installer creates.
cbpp_patchelf() {
    if cbpp_has_cmd patchelf; then command -v patchelf; return; fi
    local venv_dir="${CBPP_DATA_DIR:-$(cbpp_data_dir)}/mediapipe-venv/bin"
    if [ -x "${venv_dir}/patchelf" ]; then printf '%s\n' "${venv_dir}/patchelf"; return; fi
    local py
    py="$(cbpp_venv_python)"
    if [ -x "$py" ]; then
        "$py" -m pip install --quiet patchelf >/dev/null 2>&1 || true
        if [ -x "${venv_dir}/patchelf" ]; then printf '%s\n' "${venv_dir}/patchelf"; return; fi
    fi
    return 1
}
