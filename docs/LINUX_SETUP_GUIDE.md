# Linux Setup & Verification Guide for Local LLM Agents

This guide is written for an AI agent running locally on a Linux PC. It walks
through cleaning an existing setup, building from source, testing the installer,
verifying images flow through the bridge, cleaning SteamVR, and testing the
driver + bridge end to end.

Follow the steps in order. Each section is self-contained so you can jump to
what you need.

## Prerequisites

The PC must have:

- A Linux desktop with a compositor (X11 or a supported Wayland session —
  GNOME Wayland is **not** supported by SteamVR; use X11 or KDE/Sway/Hyprland)
- A Vulkan GPU (NVIDIA, AMD, or Intel) with working drivers
- `git`, `cmake`, `pkg-config`, a C++17 compiler (`g++` or `clang`)
- Rust (stable toolchain) for the bridge — **optional**; see "No Rust" below
- Steam + SteamVR installed (native, Flatpak, or Snap)
- FFmpeg dev packages (the driver links against system FFmpeg)

### Install build dependencies — pick your distro

**Debian / Ubuntu / Linux Mint / Pop!_OS:**

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config \
    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
    libvulkan-dev libx11-dev \
    ffmpeg git
```

**Fedora / RHEL / Silverblue:**

```bash
sudo dnf install -y \
    gcc-c++ make cmake pkgconfig \
    ffmpeg-devel vulkan-headers libX11-devel \
    ffmpeg git
```

**Arch Linux / Manjaro / Garuda:**

```bash
sudo pacman -S --needed --noconfirm \
    base-devel cmake pkgconf \
    ffmpeg vulkan-headers libx11 \
    git
```

**openSUSE Tumbleweed / Leap:**

```bash
sudo zypper install -y \
    gcc-c++ cmake pkg-config \
    ffmpeg-devel vulkan-devel libX11-devel \
    ffmpeg git
```

**Alpine Linux:**

```bash
sudo apk add --no-cache \
    build-base cmake pkgconf \
    ffmpeg-dev vulkan-dev x11-dev \
    ffmpeg git bash
```

> **Note on Alpine:** Alpine uses musl libc, not glibc. The driver and bridge
> will compile, but if you built them on a glibc system and copy the binaries
> to Alpine (or vice versa), they will not run. Always build on the target
> system.

### Install Rust (for the bridge)

The bridge (`cardboard-bridge-svc`) is a Rust binary. If Rust is **not**
installed, the driver will still build and install — only the bridge app and
its desktop entry will be skipped (see [No Rust installed](#3b-no-rust-installed)
below). To get the full setup:

```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
source "$HOME/.cargo/env"
```

### Install dxc (DirectXShaderCompiler)

The driver's HLSL shaders are compiled to SPIR-V at build time using `dxc`.
This is **not** a distro package — you must download the prebuilt binary:

```bash
DXC_VERSION="v1.9.2609"
DXC_TAR="linux_dxc_2026_09_28.x86_x64.tar.gz"
curl -fsSL -o /tmp/dxc.tar.gz \
  "https://github.com/microsoft/DirectXShaderCompiler/releases/download/${DXC_VERSION}/${DXC_TAR}"
sudo mkdir -p /opt/dxc
sudo tar -xzf /tmp/dxc.tar.gz -C /opt/dxc
DXC_BIN=$(find /opt/dxc -name dxc -type f | head -1)
sudo ln -sf "$DXC_BIN" /usr/local/bin/dxc
dxc --version
```

---

## 1. Clean the Current Setup

Remove any previous Cardboard++ installation so you start from a known state.

### Remove the driver from SteamVR

```bash
# Stop SteamVR first
pkill -x vrserver 2>/dev/null || true
pkill -x vrmonitor 2>/dev/null || true
pkill -x vrwatchdog 2>/dev/null || true
sleep 2

# Remove the driver directory
rm -rf "$HOME/.local/share/SteamVR/drivers/cardboardplusplus"

# Remove the driver from vrpathreg (SteamVR's external-driver registry)
if [ -f "$HOME/.config/openvr/openvr.vrpaths" ]; then
    echo "Check openvr.vrpaths for cardboardplusplus entries:"
    grep -i cardboard "$HOME/.config/openvr/openvr.vrpaths" || echo "  (none found)"
    # Remove any line referencing cardboardplusplus, then save
    sed -i '/cardboardplusplus/d' "$HOME/.config/openvr/openvr.vrpaths"
    echo "Removed cardboardplusplus from openvr.vrpaths"
fi
```

### Remove the bridge app

```bash
# Remove the bridge binary and desktop integration
rm -f "$HOME/.local/bin/cardboard-bridge-svc"
rm -f "$HOME/.local/share/applications/cardboardplusplus.desktop"

# Remove icons in all sizes the installer may have placed
find "$HOME/.local/share/icons/hicolor" -name 'cardboardplusplus.png' -delete 2>/dev/null || true

# Remove bridge temp artifacts (hand-frame dumps, etc.)
rm -rf "$HOME/.cache/cbpp" "$HOME/.local/share/cbpp"
rm -rf /tmp/cbpp
```

### Remove build outputs

```bash
cd /path/to/cardboardplusplus
rm -rf driver_cardboardplusplus/build
rm -rf bridge/target
```

### Clean SteamVR's driver cache

SteamVR caches driver state. Remove it so the next launch re-scans:

```bash
# Remove SteamVR's driver manifest cache (forces a re-scan)
rm -f "$HOME/.local/share/SteamVR/config/steamvr.vrsettings" 2>/dev/null || true

# If SteamVR was installed via Steam, also clear the vrpaths external-driver list
# (the file above is regenerated on next SteamVR launch)
```

**Verify the cleanup:**

```bash
# These should all be empty/absent now
ls "$HOME/.local/share/SteamVR/drivers/cardboardplusplus" 2>&1  # should fail
ls "$HOME/.local/bin/cardboard-bridge-svc" 2>&1                  # should fail
grep -i cardboard "$HOME/.config/openvr/openvr.vrpaths" 2>&1     # should be empty
```

---

## 2. Build Everything from Source

### 2a. Set the OpenVR directory

The driver needs to find SteamVR's `openvr_api.so` at build time. The compile
script auto-detects SteamVR, but you can set it explicitly if detection fails
(e.g. Flatpak or Snap installs):

```bash
# Try auto-detection first:
export OPENVR_DIR="$HOME/.local/share/Steam/steamapps/common/SteamVR"
# Or for Flatpak Steam:
# export OPENVR_DIR="$HOME/.var/app/com.valvesoftware.Steam/.steam/steam/steamapps/common/SteamVR"
# Or for Snap:
# export OPENVR_DIR="$HOME/snap/steam/common/SteamVR"
```

**How to find your SteamVR path:**

```bash
# Search common locations
for p in \
    "$HOME/.steam/steam/steamapps/common/SteamVR" \
    "$HOME/.local/share/Steam/steamapps/common/SteamVR" \
    "$HOME/.var/app/com.valvesoftware.Steam/.steam/steam/steamapps/common/SteamVR" \
    "$HOME/.var/app/com.valvesoftware.Steam/steam/steamapps/common/SteamVR" \
    "$HOME/snap/steam/common/SteamVR"; do
    if [ -f "$p/bin/linux64/openvr_api.so" ]; then
        echo "Found SteamVR at: $p"
        export OPENVR_DIR="$p"
        break
    fi
done
```

### 2b. Build the SteamVR driver

```bash
cd /path/to/cardboardplusplus
bash scripts/compile-driver-linux.sh
```

**If it fails**, check:
- `dxc` is on PATH: `command -v dxc`
- `OPENVR_DIR` points to a directory containing `bin/linux64/openvr_api.so`
- FFmpeg/Vulkan/X11 dev packages are installed (see distro section above)

**Verify the build:**

```bash
SO=driver_cardboardplusplus/build/libdriver_cardboardplusplus.so
[ -f "$SO" ] || { echo "BUILD FAILED: $SO missing"; exit 1; }

# HmdDriverFactory must be exported (SteamVR calls it via dlopen)
nm -D "$SO" | grep "T HmdDriverFactory" || { echo "FAIL: HmdDriverFactory not exported"; exit 1; }

# No undefined OpenVR symbols (all driver-API calls are inline accessors)
nm -D --undefined-only "$SO" | grep -qE "\b(VR[A-Z]|OpenVR|InitServerDriverContext)" \
    && { echo "FAIL: unexpected undefined OpenVR symbols"; exit 1; } \
    || echo "OK: no undefined OpenVR symbols"
```

### 2c. Build the bridge

**If Rust is installed:**

```bash
cd /path/to/cardboardplusplus/bridge
cargo build --release
```

This produces `bridge/target/release/cardboard-bridge-svc`.

**If Rust is NOT installed:**

Skip this step. The driver will still install; only the bridge app and its
desktop entry will be skipped by the installer. See
[Section 3b](#3b-no-rust-installed) for what to do.

### 2d. Run the driver wire-protocol tests

```bash
cd /path/to/cardboardplusplus
g++ -std=c++17 -O2 -I driver_cardboardplusplus/include \
    -o driver_cardboardplusplus/build/driver_tests \
    driver_cardboardplusplus/tests/test_wire_protocol.cpp
./driver_cardboardplusplus/build/driver_tests
# Expect: "=== All driver tests passed ==="
```

### 2e. Run the bridge test suite (only if Rust is installed)

```bash
cd /path/to/cardboardplusplus/bridge
cargo test -- --test-threads=1
# Expect: all tests pass, including the e2e suite
```

---

## 3. Test the Installer

The installer (`scripts/install-driver-linux.sh`) installs **both** the driver
and the bridge app (with a desktop entry and icon). It adapts to the system:

- **Desktop environment present** → installs `.desktop` entry and icons
- **No desktop (headless)** → skips `.desktop` and icons (driver still installs)
- **Rust installed + bridge built** → installs the bridge binary + desktop entry
- **No Rust / no bridge binary** → installs the driver, skips the bridge app,
  and tells you how to build it

### 3a. Install

```bash
cd /path/to/cardboardplusplus
bash scripts/install-driver-linux.sh
```

**What the installer does:**

| Path | Contents |
|------|----------|
| `~/.local/share/SteamVR/drivers/cardboardplusplus/` | SteamVR driver |
| `~/.local/share/SteamVR/drivers/cardboardplusplus/bin/linux64/driver_cardboardplusplus.so` | Driver shared object |
| `~/.local/share/SteamVR/drivers/cardboardplusplus/driver.vrdrivermanifest` | Driver manifest |
| `~/.local/share/SteamVR/drivers/cardboardplusplus/resources/` | Controller profile, bindings |
| `~/.local/bin/cardboard-bridge-svc` | Bridge binary (if built) |
| `~/.local/share/applications/cardboardplusplus.desktop` | App launcher entry (if DE present) |
| `~/.local/share/icons/hicolor/<size>/apps/cardboardplusplus.png` | App icon (all available sizes) |

### 3b. No Rust installed

If Rust is not installed, the installer will:
1. Install the driver (`.so`, manifest, resources) — **fully functional**
2. Skip the bridge binary and print instructions:
   ```
   Bridge binary NOT found in bridge/target/ — the desktop entry will not
   launch until you build it:
     1. Install Rust:  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
     2. Build the bridge:  cd bridge && cargo build --release
     3. Re-run this installer
   ```

**To complete the setup later**, install Rust, build the bridge, and re-run
the installer. The driver does not need to be rebuilt — only the bridge needs
to be built and the installer re-run to pick it up.

### 3c. Verify the installation

**Verify the driver layout:**

```bash
ROOT="$HOME/.local/share/SteamVR/drivers/cardboardplusplus"

[ -f "$ROOT/bin/linux64/driver_cardboardplusplus.so" ] || echo "FAIL: driver .so missing"
[ -f "$ROOT/driver.vrdrivermanifest" ] || echo "FAIL: manifest missing"
[ -f "$ROOT/resources/controller_profile.json" ] || echo "FAIL: controller profile missing"
```

**Verify the bridge app (if Rust was installed):**

```bash
[ -x "$HOME/.local/bin/cardboard-bridge-svc" ] || echo "FAIL: bridge binary missing"
[ -f "$HOME/.local/share/applications/cardboardplusplus.desktop" ] || echo "FAIL: desktop entry missing"
```

**Verify icons are installed in all sizes:**

```bash
find "$HOME/.local/share/icons/hicolor" -name 'cardboardplusplus.png' | sort
# Expect at least: 16x16, 32x32, 48x48, 64x64, 128x128, 256x256
```

**Verify the desktop entry is valid:**

```bash
DESKTOP="$HOME/.local/share/applications/cardboardplusplus.desktop"
grep "Type=Application" "$DESKTOP"
grep "Icon=cardboardplusplus" "$DESKTOP"
grep "Exec=" "$DESKTOP"
```

The desktop entry should launch `~/.local/bin/cardboard-bridge-svc` with the
`cardboardplusplus` icon. The app should appear in the launcher (application
menu) under **"Cardboard++ Bridge"**.

The desktop entry and icon live in `driver_cardboardplusplus/app/` in the
repo (separate from `resources/`, which holds SteamVR driver files only).

**If the icon doesn't appear in the launcher:**

1. Make sure icons are in the standard hicolor paths:
   ```bash
   find "$HOME/.local/share/icons/hicolor" -name 'cardboardplusplus.png'
   ```
2. The installer should have already refreshed the cache. If not, do it manually:
   ```bash
   update-desktop-database "$HOME/.local/share/applications" 2>/dev/null || true
   gtk-update-icon-cache "$HOME/.local/share/icons/hicolor" 2>/dev/null || true
   ```
3. Log out and back in (or restart the desktop session) so the launcher
   rescans `~/.local/share/applications`.

---

## 4. Verify an Image Comes Out of the Bridge

The bridge receives JPEG frames from the phone on UDP 42072 and decodes them.
When hand tracking is active, it also dumps the latest frame to a temp file for
debugging.

### Start the bridge in headless mode (no UI)

```bash
# Quit any running bridge first
pkill -f cardboard-bridge-svc 2>/dev/null || true
sleep 1

# Start the bridge headless (REST control plane on 127.0.0.1:8567)
cardboard-bridge-svc --headless &
BRIDGE_PID=$!
echo "Bridge PID: $BRIDGE_PID"
sleep 2
```

### Check the bridge is alive

```bash
curl -s http://127.0.0.1:8567/health
# Expect: {"ok":true,"app_version":"..."}
```

### Check the status endpoint

```bash
curl -s http://127.0.0.1:8567/status | python3 -m json.tool
```

Look for these fields:

- `driver_connected` — true when the driver (SteamVR) connects on UDP 42074
- `phone_connected` — true when the phone connects on UDP 42070
- `camera_connected` — true when the phone sends JPEG frames on UDP 42072
- `net_frames_decoded` — incrementing counter of decoded JPEG frames
- `hands_detected` — number of hands detected (0 if no phone camera)

### Verify image output

The bridge dumps the latest camera frame and hand-tracking data to `/tmp/cbpp/`:

```bash
ls -la /tmp/cbpp/ 2>/dev/null
# Expect: hand_frame.jpg (the raw JPEG from the phone)
#          hand_frame.txt (landmark coordinates, if hands were detected)
```

**If `hand_frame.jpg` exists and is a valid JPEG:**

```bash
# Check it's a valid JPEG (starts with FF D8)
xxd /tmp/cbpp/hand_frame.jpg | head -1
# Expect: 00000000  ffd8ffe0 ...  (JPEG SOI marker)

# Check the file size (should be non-trivial, ~10-50 KB for 256x192)
ls -l /tmp/cbpp/hand_frame.jpg

# View it (if a viewer is available)
xdg-open /tmp/cbpp/hand_frame.jpg 2>/dev/null || echo "Open the file manually to view"
```

**If `hand_frame.txt` exists, check the landmark data:**

```bash
cat /tmp/cbpp/hand_frame.txt
# Expect lines like:
#   seq=123 count=2
#   hand0 label=Left score=0.987
#    img[0] 0.5234 0.4123 0.0000
#   ...
```

**If no image appears**, the phone isn't sending camera frames. Check:

1. The phone app is running and connected to the bridge (check `phone_connected`
   in `/status`).
2. The phone's camera permission is granted.
3. Check the bridge logs:
   ```bash
   curl -s "http://127.0.0.1:8567/logs?n=50"
   ```
   Look for camera/JPEG related messages.

### Check the preview stats

```bash
curl -s http://127.0.0.1:8567/preview | python3 -m json.tool
# Expect: {"driver":{"fps":...,"bitrate_kbps":...,"frames":...,"drops":...}}
```

The `frames` counter should increment when the driver is streaming video.

### Stop the bridge

```bash
kill $BRIDGE_PID 2>/dev/null || true
```

---

## 5. Clean SteamVR and Test the Driver

### Full SteamVR clean

```bash
# Stop SteamVR completely
pkill -x vrserver 2>/dev/null || true
pkill -x vrmonitor 2>/dev/null || true
pkill -x vrwatchdog 2>/dev/null || true
sleep 3

# Remove the driver's vrsettings entry (forces SteamVR to re-read the manifest)
VRSETTINGS="$HOME/.local/share/Steam/steamapps/common/SteamVR/config/steamvr.vrsettings"
if [ -f "$VRSETTINGS" ]; then
    echo "Checking steamvr.vrsettings for cardboardplusplus:"
    grep -A3 "driver_cardboardplusplus" "$VRSETTINGS" || echo "  (no entry — will be created on launch)"
fi

# Remove the driver from vrpathreg
if [ -f "$HOME/.config/openvr/openvr.vrpaths" ]; then
    sed -i '/cardboardplusplus/d' "$HOME/.config/openvr/openvr.vrpaths"
fi

# Remove the driver directory (keep the installer to re-install)
rm -rf "$HOME/.local/share/SteamVR/drivers/cardboardplusplus"

# Clear SteamVR's log to make the next run's output easy to read
rm -f "$HOME/.local/share/Steam/logs/vrserver.txt" 2>/dev/null || true
```

### Re-install and test the driver

```bash
cd /path/to/cardboardplusplus
bash scripts/install-driver-linux.sh
```

**Register the driver with SteamVR** (if not auto-discovered):

```bash
VRPATHREG="$HOME/.local/share/Steam/steamapps/common/SteamVR/bin/linux64/vrpathreg"
if [ -x "$VRPATHREG" ]; then
    "$VRPATHREG" adddriver "$HOME/.local/share/SteamVR/drivers/cardboardplusplus"
    "$VRPATHREG" show
else
    echo "vrpathreg not found at $VRPATHREG — SteamVR should auto-discover the driver"
fi
```

**Force-enable the driver in vrsettings** (the bridge's `install_driver` does
this on Windows; on Linux, ensure the driver section is enabled):

```bash
VRSETTINGS="$HOME/.local/share/Steam/steamapps/common/SteamVR/config/steamvr.vrsettings"
if [ -f "$VRSETTINGS" ]; then
    # Check if the driver section exists; if not, SteamVR creates it on launch
    grep -A3 "driver_cardboardplusplus" "$VRSETTINGS" || echo "No vrsettings entry — SteamVR will create one on launch"
fi
```

### Launch SteamVR and check the driver loads

```bash
# Launch SteamVR
"$HOME/.local/share/Steam/steamapps/common/SteamVR/bin/linux64/vrmonitor.sh" &
sleep 8

# Check the log for the driver
LOG="$HOME/.local/share/Steam/logs/vrserver.txt"
if [ -f "$LOG" ]; then
    echo "=== Driver load lines ==="
    grep -i "cardboardplusplus\|driver_cardboardplusplus" "$LOG" | head -20
    echo
    echo "=== Any errors? ==="
    grep -i "error\|failed\|warn" "$LOG" | grep -i "cardboard\|driver" | head -10
else
    echo "vrserver.txt not found at $LOG — check SteamVR's log path"
fi
```

**Expected in the log:**

- `Loaded server driver cardboardplusplus` — the driver .so was loaded
- The driver should register a virtual HMD and controller devices
- No `Failed to open` or `Hmd Not Found` errors related to cardboardplusplus

**If the driver fails to load:**

1. Check that `driver.vrdrivermanifest` exists at the driver root:
   ```bash
   ls "$HOME/.local/share/SteamVR/drivers/cardboardplusplus/driver.vrdrivermanifest"
   ```
2. Check that the .so is at the expected path:
   ```bash
   ls "$HOME/.local/share/SteamVR/drivers/cardboardplusplus/bin/linux64/driver_cardboardplusplus.so"
   ```
3. Check that the .so has no missing shared-library dependencies:
   ```bash
   ldd "$HOME/.local/share/SteamVR/drivers/cardboardplusplus/bin/linux64/driver_cardboardplusplus.so"
   # Any "not found" lines indicate missing system libraries
   ```
4. Make sure FFmpeg dev libraries are installed (the driver links against them):
   ```bash
   dpkg -l | grep libav
   ```

### Verify the driver reports to the bridge

With SteamVR running and the bridge running, check the bridge status:

```bash
curl -s http://127.0.0.1:8567/status | python3 -m json.tool
# driver_connected should be true
# preview.frames should increment
```

---

## 6. Test the Driver + Bridge End to End

This is the full integration test: driver streams video → bridge receives it →
bridge reports stats.

### Start the bridge (headless)

```bash
pkill -f cardboard-bridge-svc 2>/dev/null || true
sleep 1
cardboard-bridge-svc --headless &
BRIDGE_PID=$!
sleep 2
```

### Confirm the bridge sees the driver

```bash
curl -s http://127.0.0.1:8567/status | python3 -m json.tool
```

Check:
- `driver_connected: true` — the driver is sending sensor/heartbeat data on UDP 42074
- `preview_driver_fps: > 0` — the driver is streaming video
- `preview_frames: incrementing` — frames are flowing

### Push settings to the driver

```bash
curl -s -X POST http://127.0.0.1:8567/settings \
    -H "Content-Type: application/json" \
    -d '{"width":2880,"height":1620,"fps":60,"bitrate":20,"encoder":"gpu"}'
```

Then verify the driver acknowledged:

```bash
curl -s http://127.0.0.1:8567/preview | python3 -m json.tool
# fps should reflect the applied settings
```

### Check the wire protocol is live

The bridge and driver communicate over these ports:

| Port | Direction | Purpose |
|------|-----------|---------|
| 42069 | driver → phone | H.264 video stream |
| 42070 | bidirectional | discovery / control (text) |
| 42071 | phone → bridge | telemetry |
| 42072 | phone → bridge | JPEG camera frames |
| 42073 | bridge → phone | TCP MediaPipe |
| 42074 | bridge → driver | sensor / skeleton data |
| 8567 | REST | bridge control plane |

Check that the driver's UDP sockets are listening:

```bash
ss -ulnp | grep -E "42069|42070|42074" || netstat -ulnp 2>/dev/null | grep -E "42069|42070|42074"
```

### Run the full e2e test suite

```bash
cd /path/to/cardboardplusplus/bridge
cargo test --test e2e -- --test-threads=1
# This runs the fake-mode suite: fake driver + fake phone + real bridge wire
```

### Stop everything

```bash
kill $BRIDGE_PID 2>/dev/null || true
pkill -x vrserver 2>/dev/null || true
pkill -x vrmonitor 2>/dev/null || true
```

---

## 7. Look at the App and Make Everything Work

### Launch the bridge UI

The bridge has a Slint UI (setup wizard + hub). Launch it from the desktop
entry or directly:

```bash
cardboard-bridge-svc
```

**First launch** shows a setup wizard:
1. **Step 0–1**: Welcome + connection status
2. **Step 2**: Install driver button (on Linux, the driver is already installed
   by the shell script; this button is Windows-focused but the status should
   show the driver as present)
3. **Step 3**: Phone connection — the wizard auto-advances when the phone
   connects

**After setup**, the hub window opens with:
- **Top bar**: connection status (driver, phone, encoder), FPS, latency
- **Stream tab**: resolution, FPS, bitrate, encoder selector, link test
- **Hand tab**: hand-tracking toggles, thresholds, detection counts
- **Controls tab**: input mode, gesture slots
- **General tab**: driver install, SteamVR launch, logs

### Verify the UI shows live data

With SteamVR + driver + phone all running:

1. **Top bar**: `driver_connected` pill should be green; FPS should show a
   non-zero value
2. **Stream tab**: FPS, bitrate, and frame counters should be live
3. **Hand tab**: if the phone camera is active, `hands_detected` should show
   0–2 and the detection count should update

### Check the app icon in the launcher

1. Open the application menu (Super/Activities, or the app launcher)
2. Search for "Cardboard++" or "Cardboard++ Bridge"
3. The app should appear with the `cardboardplusplus` icon
4. Click it to launch the bridge UI

### Common issues and fixes

**Icon doesn't appear in the launcher:**
- Ensure `~/.local/share/applications/cardboardplusplus.desktop` exists
- Run `update-desktop-database ~/.local/share/applications`
- Run `gtk-update-icon-cache ~/.local/share/icons/hicolor`
- Log out/in to refresh the launcher cache

**Bridge UI won't launch (Slint error):**
- The bridge needs a display. Run it from a desktop session, not a headless SSH
- If running over SSH, use `ssh -X` with X11 forwarding

**Driver doesn't load in SteamVR:**
- Check `ldd` on the .so for missing libraries
- Verify the manifest is at the driver root
- Check `vrserver.txt` for the actual error message
- The known SteamVR "failed to open" bug was a SteamVR-side issue (SteamVR
  2.18.x fixed it); update SteamVR if the problem persists

**No image from the bridge:**
- Check `phone_connected` in `/status` — if false, the phone isn't connected
- Check `camera_connected` — if false, the phone isn't sending JPEG frames
- Check `/tmp/cbpp/hand_frame.jpg` — if missing, no frames arrived
- Check the bridge logs via `curl /logs?n=50`

**Driver connects but no video frames:**
- Check `preview_frames` in `/preview` — if 0, the driver isn't streaming
- The driver needs a valid swap chain (SteamVR must be running with the HMD
  active)
- Check the driver log for Vulkan errors

---

## Quick Reference

```bash
# Clean everything
pkill -x vrserver; pkill -x vrmonitor; pkill -f cardboard-bridge-svc
rm -rf ~/.local/share/SteamVR/drivers/cardboardplusplus
rm -f ~/.local/bin/cardboard-bridge-svc ~/.local/share/applications/cardboardplusplus.desktop
find ~/.local/share/icons/hicolor -name 'cardboardplusplus.png' -delete 2>/dev/null
sed -i '/cardboardplusplus/d' ~/.config/openvr/openvr.vrpaths 2>/dev/null

# Build
export OPENVR_DIR="$HOME/.local/share/Steam/steamapps/common/SteamVR"
bash scripts/compile-driver-linux.sh
cd bridge && cargo build --release && cd ..

# Install
bash scripts/install-driver-linux.sh

# Test bridge (headless)
cardboard-bridge-svc --headless &
curl -s http://127.0.0.1:8567/health
curl -s http://127.0.0.1:8567/status | python3 -m json.tool
ls -la /tmp/cbpp/  # hand_frame.jpg should appear

# Test driver in SteamVR
vrmonitor.sh &
sleep 8
grep -i cardboard ~/.local/share/Steam/logs/vrserver.txt

# Run tests
g++ -std=c++17 -I driver_cardboardplusplus/include \
    -o /tmp/dt driver_cardboardplusplus/tests/test_wire_protocol.cpp && /tmp/dt
cd bridge && cargo test -- --test-threads=1
```
