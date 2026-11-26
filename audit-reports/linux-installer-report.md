# Linux installer & portability report (2026-11-25)

Companion to `linux-port-test-report.md`. This pass focused on making the
install flow match the Windows equivalent on any Linux distro, adding desktop
integration, and finding OS-fragile code in all three components.

## Installers added

One universal entry point plus component scripts, all sharing
`scripts/lib/linux-common.sh`:

| Script | Purpose |
|---|---|
| `scripts/install-linux.sh` | Distro detection (apt/dnf/yum/pacman/apk/zypper/xbps/emerge, sudo or doas), orchestration. With no flags: driver + bridge + MediaPipe + desktop entry + SteamVR registration. |
| `scripts/install-linux.sh --deps` | Installs build/runtime packages with the detected package manager. |
| `scripts/install-linux.sh --driver/--bridge/--mediapipe/--steamvr/--start` | Selects components. |
| `scripts/install-driver-linux.sh` | Rewritten: graceful SteamVR stop, backup, `.so` + manifest + resources, registration, safe-mode clear, launcher entry. |
| `scripts/install-bridge-linux.sh` | Builds (when cargo exists) and installs the bridge binary, stages sidecar + model, refreshes the launcher. |
| `scripts/install-mediapipe-linux.sh` | Per-user venv with `cv2`/`mediapipe`/`numpy`; `--system` fallback for unusual distros. |
| `scripts/install-steamvr-linux.sh` | Finds SteamVR in native/Flatpak/Snap layouts, registers the driver, clears safe mode, `--start` launches `vrmonitor.sh`. |
| `scripts/uninstall-linux.sh` | Removes driver/bridge/desktop/icon; `--purge-data` also removes the venv and wizard marker. |
| `scripts/compile-driver-linux.sh` | Now finds SteamVR automatically and bootstraps `dxc` into the user data dir if it is missing. |

Desktop integration: the `.desktop` file is installed unconditionally into
`$XDG_DATA_HOME/applications` with an absolute `Exec`/`TryExec` path (no `%h`
assumption), all six icon sizes into `hicolor`, and the icon/desktop caches are
refreshed (`gtk-update-icon-cache`, `update-desktop-database`, `xdg-desktop-menu`).

## Windows-only / OS-fragile code found and fixed

| # | File | Finding | Fix |
|---|---|---|---|
| 1 | `bridge/crates/bridge-core/src/paths.rs` | `setup.done` fell back to `/tmp` on Linux, so the wizard state was effectively random; wizard marker path not portable | `user_data_dir()`: LOCALAPPDATA → XDG_DATA_HOME → `~/.local/share/CardboardPlusPlus` |
| 2 | `paths.rs` | Windows-only defaults: DLL artifact, `ProgramFiles` driver slot, `adb.exe` | `cfg`-split artifact/driver paths, Linux ADB paths + PATH lookup |
| 3 | `paths.rs` | SteamVR root derived by walking two parents — wrong for externally registered Linux drivers | Linux returns `~/.local/share/Steam` (config root) |
| 4 | `bridge/crates/cardboard-bridge/src/core.rs` | `python_command()` never tried `python3`; installed bridge could not start the sidecar on most Linux distros | check the installer venv first, then `python3`, then `python` |
| 5 | `core.rs` | Sidecar script lookup had no path for an installed binary | added the user data dir as a candidate |
| 6 | `core.rs` | SteamVR detection used `tasklist`; start used `steam.exe -applaunch` | Linux `pgrep` detection and `vrmonitor.sh` launch with Flatpak/Snap candidates |
| 7 | `core.rs` | In-app "Install driver" always ran the Windows DLL flow | Linux delegates to `install-linux.sh --driver` |
| 8 | `core.rs`, `driver_install.rs`, `driver_deps.rs` | `curl.exe`, PowerShell unzip, vendored FFmpeg DLLs, `winget` hints | Linux uses curl/wget, system FFmpeg, package-manager hints |
| 9 | `net/mediapipe.rs` | Stale-sidecar port reclaim was Windows-only, so a hung Python on 42073 could not be cleared on Linux | `/proc/net/tcp{,6}` + `/proc/<pid>/fd` lookup, cmdline check, `kill` |
| 10 | `net/phone.rs`, `net/camera.rs` | Log hints said "Windows firewall" | platform-specific firewall hints |
| 11 | `scripts/install-driver-linux.sh` | Only warned when SteamVR was running; installer path could be a stale external registration | graceful stop and registered-path preference (in shared lib) |
| 12 | `scripts/compile-driver-linux.sh` | Required dxc already on PATH | auto-download into the user data dir |
| 13 | `.github/workflows/linux-driver.yml` | No validation of the installer scripts | added `bash -n` over all shell scripts plus installer `--help` smoke test |

Windows-only code left in place **by design**: `scripts/*.ps1`, `installer/bridge.iss`,
and the legacy `bridge-ui` crate (not the product UI). The Windows tasklist/netstat
parsers are now `cfg(windows)`-gated so they do not warn or break Linux builds.

## Verification performed

- `bash -n` on every new/changed shell script — all pass.
- `bash scripts/install-linux.sh --help` — prints component help.
- Dry run with a temporary `HOME`: driver installer places `.so`, manifest,
  resources, desktop entry and all six icon sizes; `Exec` is absolute.
- Full `bash scripts/install-linux.sh` on this host (CachyOS, pacman): detected
  distro, installed driver + bridge + MediaPipe venv + launcher + SteamVR
  registration, exit 0. Python 3.14 venv accepted `mediapipe`, `cv2`, `numpy`.
- Installed bridge binary started headless and reached
  `mediapipe model loaded, listening on 127.0.0.1:42073` +
  `bridge connected` — the staged script and venv are found correctly.
- `bash scripts/install-steamvr-linux.sh --start` launched `vrmonitor.sh`.
- `uninstall-linux.sh --purge-data` tested in a temp HOME: removed driver,
  binary, desktop entry, icons, venv and wizard marker.
- `cargo test --manifest-path bridge/Cargo.toml -- --test-threads=1`: all
  targets pass (15 bridge-core, 156 bridge lib, 14 e2e, 10 hand-sidecar, 14 wire).
- Driver wire tests still pass from the previous port work; driver `.so`
  unchanged in this pass.

## SteamVR not opening (follow-up)

Two separate causes were found in `vrserver.txt`, `vrmonitor.txt` and process state:

1. **Bridge blocked launches on a zombie.** `start_steamvr()` used `pgrep -x` to
   decide whether SteamVR was already running. A dead `vrmonitor` child that had
   not been reaped still matched, so the bridge refused to launch SteamVR.
   `core.rs` now reads `/proc/<pid>/stat` and ignores processes in state `Z`, and
   owns the spawned `vrmonitor` in a reaper thread (`child.wait()`) so the
   kernel reaps it when SteamVR exits instead of leaving a zombie.
2. **Driver could not load under Steam's runtime.** `vrserver.txt` showed
   `libavformat.so.63: cannot open shared object file`. The driver linked against
   host FFmpeg but Steam launches vrserver inside its own runtime, where
   `/usr/lib` is not on the loader path. Fixes: `CMakeLists.txt` adds
   `$ORIGIN:/usr/lib:/usr/lib64:/run/host/usr/lib:/run/host/usr/lib64` to RUNPATH,
   and `install-driver-linux.sh` copies the five FFmpeg shared libraries next to
   the `.so`. A fresh launch now logs `Loaded server driver cardboardplusplus`
   with no library error, and the compositor has no texture-import failures.

**Headset was off / "Room Setup" (follow-up):** with the driver loaded the
headset still stayed at `Room Setup (C201)` / `Searching...`. Two causes were
fixed:
- `GetPose()` returned an invalid out-of-range pose whenever no phone sensor
  packet arrived. On Linux it now publishes a fixed identity pose at 1.6 m so
  the HMD stays present; real rotations take over as soon as they arrive.
- SteamVR had no play area for universe 2. A minimal
  `resources/chaperone_info.vrchap` is shipped and its path published through
  `Prop_DriverProvidedChaperonePath_String`.
- Safe-mode blocks were also being restored from `steamvr.vrsettings.backup`;
  both the shell helper (`cbpp_enable_driver_in_vrsettings`) and Rust
  `force_driver_enabled()` now clear the flag in every `steamvr.vrsettings*`
  copy in both Steam config directories.

Verified after those fixes: `HMD chaperone: .../resources/chaperone_info.vrchap`
in `vrserver.txt` and `SteamVRSystemState_Connecting -> SteamVRSystemState_Ready`
in `vrmonitor.txt`, with no `Headset Not Detected` or `Room Setup` alert in that
run. Bridge still reports 60 fps preview with no phone.

**Image-in-headset investigation (not yet fully working):** with a VR scene app
(VRChat) running, the compositor now calls `CreateSwapTextureSet`, `SubmitLayer`
and `Present`. Fixes made along the way:
- Linux compositor passes 1-based swap-set indices instead of the shared
  handles we publish; `ResolveSwapSet()` maps those back to our sets.
- `Present` receives `syncTexture = 0` from this SteamVR build; a null sync
  handle now means "no wait", not "drop the frame".
- `Prop_Hmd_AllowsClientToControlTextureIndex` is explicitly false so SteamVR
  does not own the texture index.
- Debug logging added for first SubmitLayer/Present/encode stages.

**RESOLVED — frames are now out of SteamVR through Vulkan.** The stall was in
`VulkanCompose::FindOrImportEye()`: `vkGetMemoryFdPropertiesKHR()` and the
`VkImageDrmFormatModifierExplicitCreateInfoEXT` chain on the compositor's
dma-buf fd hung `vkCreateImage()` inside the encode thread. Both were removed;
the eye image now uses plain `VkExternalMemoryImageCreateInfo` with the
dma-buf handle type and selects a memory type from the image requirements.

Verified with VRChat as the scene app: `VK-COMPOSE: frame=2`, `Encode: compose
ok / readback ok / convert ok / finish ok`, `UDP: first encoded packet
size=7206 keyframe=1 preview=1`, and a UDP capture on 42069 started before the
game launch recorded **7 datagrams** (1400-byte video chunks plus smaller
trailing packets). The capture ends when VRChat exits (it needs a login), so a
longer stream could not be held here. The bundled `steamtours.sh` scene app
cannot substitute because it dies on missing `libpng12.so.0`.

**Errors seen during the user's VRChat session (23:43–23:44):** `vrcompositor.txt`
showed `CreateImageWithFlags: failed to create vulkan image (-1000158000)` =
`VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT`, so SteamVR could not
create its own dmabuf textures; `vrmonitor.txt` ended with `SteamVR Fail (-301)`.
Attempts to advertise the GPU's real modifier and real plane stride made
`ImportDmabuf()` fail outright. Restored the combination that works here:
SteamVR's supported modifiers with a linear stride; the compose fix (no
`vkGetMemoryFdPropertiesKHR`, no explicit modifier descriptor in
`FindOrImportEye`) remains.

The mono fallback and its `CBPP_STEREO` opt-out flag were removed at the
user's request. The driver now always uses both submitted eye textures.

**Stereo fixed:** the remaining cause was `GetNextSwapTextureSetIndex()`. It
advanced only the left set and derived the right index from it, so SteamVR
rendered the right eye into a slot the driver never read. Following ALVR, each
eye's swap set is now advanced independently. Combined with the sync-file
semaphores and `NewSharedVulkanImage` swap textures, the captured frame
(`/tmp/opencode/frame_idx.png`) shows both eyes with correct parallax — true
stereo, no mono duplication.

**ALVR research / stereo attempt:** ALVR PR #3398 switched Linux to the same
`IVRDriverDirectModeComponent` path: `NewSharedVulkanImage` with
`VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` and `TransferSrc|Sampled|InputAttachment`
usage, `RefResource` + `ReceiveSharedFd`, `dup()` every fd handed to Vulkan
(import consumes fds), and `DMA_BUF_IOCTL_EXPORT_SYNC_FILE` waits on both eyes
before rendering. I applied all of those in our driver (`CreateSharedEye`,
sync-file wait in `VulkanCompose::Compose`). Compositor dmabuf import errors are
gone, but inspected frames still show the right half black while the left half
renders. True stereo remains unresolved; the mono fallback is still the default.

**Stereo status:** replaced our exported dma-bufs with SteamVR-allocated
`NewSharedVulkanImage` textures (`CreateSharedEye`), imported into our Vulkan via
`RefResource`/`ReceiveSharedFd`. This removed the compositor's
`Failed to create new image from dmabuf` errors. Inspecting the resulting PNG
still showed the right half black while the left half rendered the real SteamVR
home scene. The compositor hands `Present` a null sync handle, so there is no
semaphore to wait on before reading the second eye. True stereo therefore still
does not work; the default remains the mono fallback (both halves from the
working eye), which produces a complete, uncorrupted image. `CBPP_STEREO=1`
opts out for debugging.

**User-facing state (Nov 26):** no custom SteamVR build is used. SteamVR is
the stock 2.18.4 install (build 1792185270); no Vulkan layer or patched
`vrcompositor` is involved. The non-default items are: our driver `.so`
installed into the registered external driver directory, the SteamVR setting
`hmdAllowsClientToControlTextureIndex = false`, cleared safe-mode flags, the
bridge/MediaPipe stack installed, and `dxc` downloaded into the user data dir.
Test-only env hooks (`CBPP_CPU_CONVERT`, `CBPP_DUMP_NV12`, `CBPP_STEREO`) were
removed from the driver source.

VRChat opened in desktop mode because its log showed `[OpenVR] Could not
initialize OpenVR. Init_InterfaceNotFound` — SteamVR was not running when it
initialized. The Steam launch option `-vrmode steamvr` had first been written
while Steam was running and was silently discarded when Steam rewrote
`localconfig.vdf`. Steam was fully shut down, the option was written into the
`Apps` block for app 438100, and Steam was relaunched; the option then survived
a restart. Backup: `localconfig.vdf.bak-cbpp`.

Verified after that: VRChat's log showed
`Arg: -vrmode`, `Arg: steamvr`, `StartVRSDK: OpenVRLoader`, and
`[SteamVR] Initialized. Connected to cardboardplusplus(...) : CardboardPlusPlus
: CBPP_VIRTUAL_HMD_001` — i.e. VR mode, not desktop.

For launches from the Steam UI, `-vrmode steamvr` alone is not enough: SteamVR
must already be running when VRChat initializes. `autolaunch: true` in
`steam.app.438100.vrappconfig` did not start it from a command-line launch. The
working fix is a Steam launch option using `%command%`:
`/home/anonymous/cardboardplusplus/scripts/steamvr-vrchat-launch.sh %command% -vrmode steamvr`.
The wrapper starts SteamVR, waits for Ready, then execs the original command.
Verified from a plain `steam -applaunch 438100`: the console log showed the
wrapper in the launch command, SteamVR was running, and VRChat logged
`Arg: -vrmode`, `StartVRSDK: OpenVRLoader`, and `[SteamVR] Initialized.
Connected to cardboardplusplus(...)`.

`scripts/play-vrchat-vr.sh` clears safe-mode flags, starts SteamVR if needed,
waits for a fresh `SteamVRSystemState_Ready` with our driver loaded, then
launches VRChat. It does not rely on a stale "Ready" line from an earlier
session.

**Image corruption fixed (Nov 26, inspected actual PNG frames).** Captured the
localhost 42069 H.264 stream to `/tmp/opencode/frame*.png` and read it directly:
the frame was top-half pure black, bottom-half pure green. Root cause in
`nv12_cs.hlsli`: the chroma row stride used `w >> 3` uints (960 bytes) instead of
`w >> 2` (1920 bytes), so chroma was packed into the first half of the plane and
the rest stayed zero (green in YUV). Corrected the stride; the raw NV12 dump now
has no zero chroma bytes and the decoded frame shows the real scene.

The remaining black right eye was traced to SteamVR failing to import the second
dma-buf swap set (`VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT`). Until
that handoff is fixed, `Present()` defaults to sampling the working eye into both
SBS halves (`CBPP_STEREO=1` opts out). The inspected frame now shows a complete,
non-corrupted SBS image (mono) instead of green/black halves.

**Nov 26 follow-up — sustained frames fixed.** Two more faults were found:
- A stale Source-engine lock (`/tmp/source_engine_*.lock`) and a half-finished
  tutorial launch left SteamVR reporting
  `VRApplicationError_LaunchInProgress`. Removing the lock and restarting
  SteamVR let `vrmonitor://runapp/openvr.tool.steamvr_tutorial` launch cleanly.
- `FindOrImportEye()` passed the compositor's dma-buf fd straight to
  `vkImportMemoryFdKHR`, which **consumes** it. After the first frame the fd
  was closed, `vkAllocateMemory(eye)` failed and Vulkan compose disabled itself
  permanently (`Vulkan compose unavailable`). It now imports `dup(fd)`.

Verified with the tutorial running: no `VK-COMPOSE disabled`, frame counter
climbing past 2100, and bridge `/status` showing `preview_frames: 2945` with
`driver_connected: true`, `preview_drops: 0`, 60 fps. No new `LaunchInProgress`
or `SteamVR Fail` alerts.

**Current verified state:** with a capture bound to UDP 42069 before launching
VRChat, 7 datagrams were received (five 1400-byte video chunks plus 211/76-byte
packets), and `UDP: first encoded packet size=7206 keyframe=1 preview=1` was
logged. The stream ends when VRChat exits (login required), so sustained output
could not be measured here. Compositor dmabuf texture errors still appear in
SteamVR's own log; they are not fatal to our encode/send path but indicate the
handoff is not fully clean.

**FPS counter without a phone:** a clean run (bridge headless + SteamVR started
via `install-steamvr-linux.sh --start`, **no phone app and no simulated
telemetry**) was polled every 5 s. `preview_driver_fps` stayed at **60** and
`preview_drops` at **0** for all ten samples, with `phone_connected: false`.
The driver's UDP preview target to `127.0.0.1:42069` is always on, so the bridge
decode works without the phone. The `driver_connected` pill dropped after ~30 s
in this environment because a second SteamVR server instance owns UDP 42070 and
does not answer `BRIDGE_HELLO`; video/STATS from the first instance continue.

**Live stream verified after both fixes:** bridge `GET /status` reported
`driver_connected: true`, `preview_driver_fps: 60`, `preview_drops: 0`, with an
`ffmpeg -f h264 ... -f rawvideo` decode process reading UDP 42069. A direct
probe of UDP 42070 returned `BRIDGE_ACK v1 128` and `BRIDGE_STATS`, and the
driver's `hasQ=1` once phone rotation packets were supplied. No zombies
remained.

A later Steam-client launch exposed a deeper variant: `libdvdnav.so.4` and then
`libleancrypto.so.1.9`. `DT_RUNPATH` is not inherited by dependencies, so
patching only the five direct FFmpeg libraries was insufficient. The installer
now walks the entire `ldd` closure, stages each host library beside the driver
and patches its RUNPATH with `patchelf` (installed by `--deps` or via the venv).
Verified with `steam -applaunch 250820`: the driver loads with no missing-library
error, the compositor starts, and the bridge reports 60 fps preview with 0 drops.

`--deps` is now the default for `install-linux.sh`: with no arguments it installs
the distro build/runtime packages first, then all components. `--no-deps` skips
only that package step. Verified with a stubbed `sudo`: a plain run on CachyOS
called `pacman -Syu --noconfirm --needed base-devel cmake pkgconf ffmpeg ...`.

## Curl-install / no-checkout bug (follow-up)

**Symptom:** the bridge's in-app "Install driver" button reported
`repo root not found, run the installer from the checkout`.

**Cause:** the Linux branch of `install_driver_files()` looked for
`driver_cardboardplusplus/driver_cardboardplusplus.sln` via `repo_root()` and then
ran `scripts/install-linux.sh --driver` from that checkout. That only works in a
git clone. A user who fetched a single script with `curl` has no checkout, no
`scripts/` directory and no `.sln` marker, so the install aborted before copying
anything. The Windows flow never had this dependency — it uses paths baked into
the installed binary.

**Fix (self-contained install):**
- `scripts/install-bridge-linux.sh` stages a driver bundle in
  `~/.local/share/CardboardPlusPlus/driver/` (the `.so`, manifest and resources).
- `core.rs` Linux install now copies from (in order) `CBPP_DRIVER_SO`, a repo
  build when present, or the staged bundle — then backs up, atomically swaps
  the `.so`, writes the baked-in resources, clears the safe-mode flag and calls
  `vrpathreg.sh adddriver`. No repo, shell script or current directory needed.
- `paths.rs` gained `register_driver()` so registration works from the installed
  binary; `install-driver-linux.sh` and `install-bridge-linux.sh` accept
  `CBPP_DRIVER_SO` / `CBPP_BRIDGE_BIN` for curl-downloaded artifacts.
- Added `linux_driver_install_works_without_repo_scripts` unit test: with a
  temporary HOME and no checkout artifact it installs a valid driver `.so`.

## Tutorial reset

`~/.local/share/CardboardPlusPlus/setup.done` was absent and was explicitly
removed; the first-launch wizard will open at step 0 on next launch. The in-app
"Back to setup wizard" button is unchanged.

## What you can test

1. `bash scripts/install-linux.sh --deps` on a clean machine of each distro you
   care about (apt-, dnf-, pacman-, apk-, zypper-based), then
   `bash scripts/install-linux.sh`. Confirm no manual package step is needed.
2. Open the applications menu: "Cardboard++ Bridge" should appear with the icon
   and launch without a terminal.
3. First launch should show the setup wizard from step 0. Walk through driver
   install, SteamVR launch and phone connection.
4. With SteamVR running, click the bridge's in-app "Install driver": on Linux it
   should now stop SteamVR, reinstall the `.so` and report success instead of
   looking for a DLL.
5. Kill any process holding TCP 42073, start the bridge, and confirm the
   sidecar is reclaimed/restarted (Linux `/proc` reclaim path).
6. Test `bash scripts/uninstall-linux.sh`, then reinstall with `--all`.
7. Flatpak/Snap Steam layouts: run `install-steamvr-linux.sh --start` and check
   it finds `vrmonitor.sh` and registers the driver.
8. No-checkout install: start the installed bridge from any directory (not the
   repo) and click "Install driver" — it should install from the staged bundle
   with no `repo root not found` message. Also try `CBPP_DRIVER_SO=/path/to.so`
   with a downloaded artifact.
9. SteamVR launch: after the installer, open SteamVR from the bridge button or
   `bash scripts/install-steamvr-linux.sh --start`; the driver should load with
   no `libavformat.so.63` error and the compositor should start. If SteamVR was
   previously killed mid-launch, restart the bridge once to clear any zombie.
10. Windows: not re-verified in this pass. The `cfg`-gated changes should be
   inert, but a Windows CI run is the real check.
