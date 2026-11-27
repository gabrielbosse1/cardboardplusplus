# Linux port — test, fix, and verification report (2026-11-25)

Branch: `linux-port` @ `5a30d7b`. Host: CachyOS x86_64, GCC 16.2.1, FFmpeg 63,
Vulkan 1.4 / RADV on AMD RX 6600M, X11, SteamVR 2.17.10 installed. No HMD or
phone connected. Windows verification was skipped at the user's request.

## Outcome

The Linux driver now builds, installs, loads in SteamVR, initializes the full
Vulkan compose + NV12 + encoder + UDP + DirectMode pipeline, and talks to the
bridge over UDP and shared memory. Final live run: `driver_connected: true`,
`preview_driver_fps: 60`, `preview_drops: 0`, `shm: attached`, handshake
established, no driver errors and no crash dumps.

## Defects found and fixed

| # | File:line | Severity | Defect | Fix |
|---|---|---|---|---|
| 1 | `driver_cardboardplusplus/include/shaders/compose_ps.hlsli:19` | high | vector ternary rejected by dxc, so Linux could not build | component-wise `lerp` with a bool→float cast, valid in both FXC and dxc |
| 2 | `driver_cardboardplusplus/cmake/EmbedSpirv.cmake:4-7` | high | generated header had a double comma after every word and byte-swapped every SPIR-V word | one regex over 4-byte groups reversing bytes, then strip the trailing comma |
| 3 | `driver_cardboardplusplus/include/shaders/compose_ps.hlsli`, `nv12_cs.hlsli`, `CMakeLists.txt:109` | high | dxc descriptor bindings (0/0/0/1 and 0/0/0) did not match C++ layouts (0/1/2/3 and 0/1/2) | `CBPP_VK_BINDINGS` define in the dxc invocation only; shader resources use explicit `[[vk::binding(n)]]` behind that macro |
| 4 | `driver_cardboardplusplus/CMakeLists.txt:55` | high | `src/HmdDriver.cpp` missing from the Linux source list → `_ZTV9HmdDriver` and `HmdDriver::RunFrame` undefined; `.so` linked but SteamVR `dlopen` failed | added the source; CI now checks for unresolved driver symbols |
| 5 | `driver_cardboardplusplus/src/VulkanCompose.cpp:341-344,501-502` | high | embedded SPIR-V 1.3 was overwritten to 1.5; RADV crashed inside `vkCreateGraphicsPipelines` and `vkCreateComputePipelines` | removed the version rewrite (1.3 is valid for Vulkan 1.2) |
| 6 | `driver_cardboardplusplus/src/DirectModePosix.cpp:30` | high | `vkGetPhysicalDeviceMemoryProperties` fetched with `GetDeviceProc` (instance-level command) → all swap-set creation failed | use `GetInstanceProc` |
| 7 | `driver_cardboardplusplus/src/DirectModePosix.cpp:161-172` | high | `DestroySwapTextureSet` erased the map entry that its iterator pointed to, then reused the invalidated iterator → SteamVR crash and the bridge heartbeat dying | erase by key |
| 8 | `driver_cardboardplusplus/src/HmdDriver.cpp:411` | low | `%ls` used for a `const char*` SHM name, so the log printed an empty path | `%s` |
| 9 | `bridge/crates/bridge-shm/src/ring.rs:9` | high | `RegionHeader` had default Rust layout: `write_seq` at offset 80 while `BridgeProtocol.h`/fakes use 24, so telemetry was never drained | `#[repr(C)]` |
| 10 | `bridge/crates/bridge-core/src/driver_install.rs:306-310` | med | Windows-only path assertion failed on Linux, stopping the suite before e2e | platform-specific assertions |
| 11 | `bridge/crates/cardboard-bridge/tests/e2e/harness.rs:41-106` | med | readiness probe panicked on the first connection refused before the spawned bridge had bound REST | added a non-panicking `try_request` used by `healthy()` |
| 12 | `scripts/install-driver-linux.sh:37` | med | installer wrote to `~/.local/share/SteamVR/drivers/`, but SteamVR loads the externally registered driver from `~/.local/share/CardboardPlusPlus/drivers/`; fresh builds were ignored | query `vrpathreg.sh` and prefer the registered path |
| 13 | `.github/workflows/linux-driver.yml` | med | CI only checked OpenVR symbols, so defect 4 passed CI | added a check for unresolved driver-defined C++ symbols |

## Verification performed

- `cmake` configure + build (Release) — success.
- `spirv-dis` on all three generated modules: graphics `Bounds=0, samp=1, leftEye=2,
  rightEye=3`; compute `Params=0, srcTex=1, outBuf=2` — matches the C++ layouts.
  `spirv-val` passes; embedded arrays are byte-identical to the `.spv` files.
- `nm -D`: `HmdDriverFactory` exported, no undefined OpenVR or driver-defined symbols.
- Driver wire tests: all pass.
- `cargo test --manifest-path bridge/Cargo.toml -- --test-threads=1`: all targets
  pass, including 14/14 e2e tests.
- Install script dry run (`HOME=<tmp>`): .so, manifest, resources, desktop entry
  and all six icon sizes placed correctly.
- Real install: fresh `.so` checksum matches the build and lands in the path
  SteamVR actually loads (old `.so` backed up).
- SteamVR live (bridge headless + `vrmonitor`): driver loaded from the installed
  path; Vulkan device ready on RADV; compose pipelines and NV12 ready; libx264
  Annex-B encoder ready; UDP 42069/42070/42074 bound; DirectMode created two
  1440x1620 swap sets of three dma-bufs; SHM region `/cardboard_pp_bridge`
  attached by the bridge; handshake, version 135, continuous `BRIDGE_ACK`/`BRIDGE_STATS`.
  Final status: `driver_connected: true`, 60 fps preview, 0 drops. No new crash
  dumps and no driver errors in the final run.

## Still unverified / environment limits

- No HMD is attached and no phone is connected, so head pose, SBS image on the
  phone, and the sensor path (42071/42074) could not be exercised end to end.
- `h264_vaapi` GPU encoding stays untested; live logs show libx264 software.
- MediaPipe's Python sidecar cannot start here (`cv2` missing); the bridge
  degrades to preview-only, as designed.
- SteamVR safe-mode blocks were cleared in both SteamVR config copies after
  testing so the freshly installed driver loads on the next launch.
- Windows verification skipped (per instruction); shader changes were written
  to remain valid for FXC, but no MSVC build was run.

## State left on the machine

- Fixed driver installed at
  `~/.local/share/CardboardPlusPlus/drivers/cardboardplusplus/bin/linux64/driver_cardboardplusplus.so`
  with timestamped backups of the previous `.so`.
- Bridge debug binary installed to `~/.local/bin/cardboard-bridge-svc`.
- Generated `DriverVersion.h` restored to the committed version; no build-time
  version stamp left in the tree.

## Follow-up: "SteamVR Fail" and "headset deactivated"

**Symptom:** SteamVR opened with an error and the headset setup showed the
Cardboard++ HMD as deactivated. PC-only test (no phone app), confirmed by the user.

**Evidence:** `vrcompositor.txt` reported
`CSharedImageLinux::ImportVulkanImage Invalid texture, width: 0 height: 0`,
`Failed to open shared resolve texture for driver direct mode`, and
`Failed to start compositor: VRInitError_Compositor_OpenDriverDirectModeResolveTextures`.
`vrmonitor.txt` logged `SteamVR Fail (476)`/`(475)`.

**Cause:** `CreateSwapTextureSet()` published each exported dma-buf **fd** as the
`SharedTextureHandle_t`. SteamVR-for-Linux cannot import a raw fd from a driver:
it expects a handle created by `IVRIPCResourceManagerClient::ImportDmabuf()`,
which carries the width/height/format/modifier. The compositor dereferenced the
fd as a texture descriptor and got zeros, so direct mode never started. This was
the unresolved "swap-set handle convention" item in `docs/LINUX_PORT.md`.

**Fix:**
- Added `driver_cardboardplusplus/include/LinuxDmabuf.h` with Valve's current
  `DmabufAttributes_t` and `IVRIPCResourceManagerClient_003` vtable (the vendored
  OpenVR 1.14 header predates them).
- Swap images are created with `VkExternalMemoryImageCreateInfo` plus the GPU's
  supported DRM modifiers (`VkImageDrmFormatModifierListCreateInfoEXT`).
- Each image is registered with `ImportDmabuf()`; the returned shared handle is
  published in `SwapTextureSet_t`, and `UnrefResource()` releases it on destroy.
  `unTextureFlags` is now initialized.

**Verified:** driver loads, `imported dmabuf ... handle=...` and swap sets are
created; compositor log has **no** texture-import or start failures after the
fix. SteamVR then shows only `Room Setup (C201)` — a normal first-time calibration
prompt, not a driver failure. With a simulated phone feeding `0x12` rotation
packets, the `Searching...` warning is removed and the driver's `hasQ=1`.

**What this means for PC-only testing:** without the phone app there is no head
pose, so SteamVR will keep the headset in `Searching`/deactivated state by design.
That is not a driver error. After the fix the remaining steps are: connect the
phone, complete SteamVR room setup once, then verify the image in the headset.
