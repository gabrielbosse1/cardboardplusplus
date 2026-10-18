# GOAL — Vulkan-ize the driver, then run it on Linux

## Why the direct port failed (read before touching Vulkan code)

- On Linux, SteamVR hands the driver **Vulkan** image handles in SubmitLayer,
  not DXGI shared handles (openvr issue #1884, Feb 2025). The compositor
  allocates its own texture sets in `CSharedTextureSetSHM` and the Linux
  vrclient hands those out. This is the reason "port to Vulkan directly"
  stalled.
- SteamVR 2.5.1+ (May 2024) DOES support `IVRDriverDirectModeComponent` on
  Linux, so direct mode is the right path — as long as every texture the
  driver touches is a Vulkan image.
- Strategy: on Windows, keep D3D11 as the owner of swap textures and convert
  the *pipeline stages* one at a time to Vulkan via
  `VK_KHR_external_memory_win32` (import the same DXGI handles into Vulkan).
  When all stages are Vulkan, the Windows code becomes the reference and the
  Linux build reuses it unchanged.
- Current D3D11 touch points:
  `HmdDriver.cpp:38` (device creation), `DirectMode.cpp` (swap sets + sync
  keyed mutex), `VideoEncoderShaders.cpp` (SBS compose), `VideoEncoder*.cpp`
  (readback + BGRA→NV12 + FFmpeg AMF-wrap of D3D11VA), `EncoderSetup.cpp`.
  Socket code (UDP 42069/42070/42074), BridgeServer (SHM), Discovery, Hand
  are Windows APIs but not D3D — they convert with the Linux build scripts,
  not with Vulkan.

**Verification loop between slices:** build → install driver only
(`scripts/install-driver.ps1`) → restart SteamVR (driver restart) → ask the
user "is the image working on the phone?" → only when yes, touch the next
slice. The bridge is a separate Windows build in this phase; do NOT restart,
rebuild, or change it during slices 1–6 (only the driver talks to it; make
sure we don't change the SHM or UDP contracts).

---

## Slice 0 — Done
- [x] Write GOAL.md.

## Slice 1 — Vulkan device scaffolding inside the driver (Windows, D3D still owns swap textures)
- [x] Add `VulkanLoader.h/.cpp` (shipped as `include/VulkanContext.h` +
  `src/VulkanContext.cpp`): LoadLibrary vulkan-1.dll, 1.1 instance with
  `VK_KHR_external_memory_capabilities`, physical device picked by D3D
  adapter LUID match, graphics+compute queue created; device extensions
  enabled only when present (`external_memory_win32`, `keyed_mutex`
  recorded as flags for slice 2). Non-fatal on any failure.
- [x] Log the Vulkan device name in the driver log on Activate
  (HmdDriver.cpp `InitializeVulkan()`).
- [x] Gate: driver builds (`scripts/compile-driver.ps1`), all wire tests
  pass, driver installed via `scripts/install-driver.ps1`.
- [x] Gate verified live: SteamVR restarted, driver reconnected, preview running
  at 60 fps; log shows `matched D3D adapter LUID on AMD Radeon RX 6600M`,
  `ext-memory-win32=1 keyed-mutex=1`, `device ready (…, queue family 0)`.
- [x] Ask user: "image working?" (confirmed)

## Slice 2 — Import a D3D texture into Vulkan + keyed-mutex probe (interop stage)
- [x] Import the encoder's `m_pConversionRT` (B8G8R8A8_UNORM, created with
  `D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX`, legacy DXGI shared handle) into a
  Vulkan image via `VkExternalMemoryImageCreateInfo` +
  `VkImportMemoryWin32HandleInfoKHR` (KMT bit). `VulkanInterop.h/.cpp`:
  Init/AttachShared/ProbeCopy/Detach/LazyInit/Disable.
- [x] Keyed mutex on Vulkan side via `VkWin32KeyedMutexAcquireReleaseInfoKHR`
  pNext on `VkSubmitInfo` (key 0, 200ms acquire); D3D side acquires in
  `ComposeSBSGPU`, releases before return, `ReleaseSync(0)` after RT creation.
- [x] Probe: `vkCmdCopyImageToBuffer` → host-visible buffer (HOST_VISIBLE
  selection with cached/coherent preference, `vkInvalidateMappedMemoryRanges`
  when non-coherent) → FNV hash logged as `[VK-PROBE: frame=N hash=…]`
  (first 2 frames + every 300th). One failure → `Disable()`; stream stays D3D-only.
- [x] Gate verified live: SteamVR restarted, `VK-PROBE: image attached 1920x1072`,
  frame hashes appearing and changing (frame 1..600: 6 changes, no disable,
  no device lost), driver reconnected, preview 60 fps.
- [ ] Ask user: "image working?"

## Slice 3 — Readback via Vulkan (replace D3D11 staging CPU copy)
- [x] Import `m_pConversionRT` into Vulkan and copy it to a host-visible
  Vulkan buffer (`VulkanInterop::ProbeCopy`: one-time layout barrier,
  `vkCmdCopyImageToBuffer`, fence wait, invalidate when non-coherent).
- [x] `ReadbackToBuffer` hands the mapped host pointer to `SwsConvert`
  (`m_pSwsSrc`) instead of copying into the D3D staging texture; the D3D
  staging path stays as the per-frame fallback when the interop probe fails.
- [x] `SwsConvert` untouched, as specified.
- [ ] Ask user: "image working?"

## Slice 4 — BGRA→NV12 conversion on Vulkan compute (replace sw_scale)
- [x] Compute pass (`VulkanCompose::SetupNv12`/`ToNv12`): reads the natively
  composed frame and writes packed NV12 into a host-visible buffer; the encoder
  frame points straight at it, so `SwsConvert` only runs when the Vulkan path
  did not produce a frame for that one.
- [x] 8 pixels per thread (2 Y words + 2 UV words). Two lowerings forced this:
  `RWByteAddressBuffer.Store` becomes a 32-bit store at `addr>>2`, and this
  shader maps to binding 0 = params UBO, 1 = sampled image, 2 = storage buffer.
- [x] BT.601 limited range matching swscale: `16 + 65.481R + 128.553G +
  24.966B`. The coefficients are already scaled for 0..1 input; dividing by 255
  again silently produced Y=16 everywhere, i.e. an all-black frame.
- [x] Verified by dumping GPU and CPU NV12 for the same frame and converting
  both with ffmpeg: Y plane mean abs difference 0.5/255, chroma within a couple
  of levels (box average vs swscale's chroma filter); the difference image is
  empty apart from a hairline at the horizon.
- [x] Live: `VK-COMPOSE frame=…`, 235 KB keyframes, preview 60 fps / 0 drops,
  no D3D fallback in the log.
- [ ] Ask user: "image working?"

## Slice 5 — SBS compose on Vulkan (replace D3D11 shaders)
- [x] Shader conversion: vendored dxil-spirv (`lib/dxil-spirv/dxbc-spirv.lib`
  + hand-built `dxil-spirv-sbv.lib`, headers under `include/dxil-spirv/`) and
  `DxbcToSpv.cpp` wrapper (HLSL → D3DCompile → DXBC → SPIR-V, once per encoder
  init). `SpirvBuilder` emits SPIR-V 1.6, which needs a Vulkan 1.3 driver, so
  the header version is retargeted to 1.5 (all emitted capabilities are core
  there) to run on Vulkan 1.2 hardware. `VulkanContext` enables the matching
  device features (vulkanMemoryModel + device scope, shaderDrawParameters,
  bufferDeviceAddress).
- [x] Vulkan render pass with blit + per-layer bounds CB, replicating
  `VideoEncoderShaders.cpp` (blit VS/PS, layer blend, bounds CB) —
  `VulkanCompose.cpp`: two pipelines (opaque + alpha), one descriptor set per
  layer, RT imported through `VulkanInterop`.
- [x] Cross-API handoff: eye layer copies became `MISC_SHARED_KEYEDMUTEX`;
  the Vulkan submit carries `VkWin32KeyedMutexAcquireReleaseInfoKHR` for the RT
  and every keyed eye memory, the present-thread copies acquire/release key 0
  around the copy, and the D3D fallback acquires the eye keys on the encoder's
  own device.
- [x] The D3D11 `ComposeSBS` path stays as runtime fallback: a Vulkan failure
  composes that frame with D3D and keeps the pipeline for the next frame
  (`vkQueueSubmit` failures are treated as transient).
- [x] Native Vulkan compose target: `VulkanCompose` owns its own
  `VkImage` (COLOR_ATTACHMENT|SAMPLED|TRANSFER_SRC) and renders there, so the
  Vulkan path never draws into imported D3D memory. The D3D11 conversion RT is
  written only on demand via `CopyToRt()` (probe readback, encoder fallback,
  bridge preview), guarded by the RT keyed mutex.
- [x] EncodingThread resolves Vulkan images via KMT import of the D3D shared
  handle (same effect as a Vulkan-native allocation; native allocation stays
  open).
- [x] Gate verified live: SteamVR restarted, `VK-COMPOSE: frame=N layers=1`
  streaming at encode rate, probe hash/mean changing, preview 60 fps / 0 drops,
  encoded frames non-trivial (keyframes ~230 KB). Frame-dump check confirmed a
  correct side-by-side stereo image (both eyes, aligned, no artifacts).
- [x] Long-session crash fix (vrserver `c0000005` after ~3 h): three defects
  found and fixed. (1) `VulkanInterop::Disable()` destroyed the imported RT
  image/memory on any runtime probe failure while `VulkanCompose` still held an
  image view inside a live framebuffer — runtime failures now go through
  `StopProbing()`, which only stops the diagnostic copy, and `Compose()` tears
  itself down if the interop image ever disappears. (2) The eye-import cache was
  keyed on the `ID3D11Texture2D*` alone; a recreated layer copy reuses the
  address, so Vulkan rendered into dead memory — the key is now
  (pointer + shared handle). (3) That cache was unbounded, leaking a
  full-resolution GPU image per stale layer pair; `PruneAux()` keeps only the
  eyes the last frame used.
- [x] `CARDBOARD_VULKAN_COMPOSE=0` env var disables the Vulkan compose at
  runtime (falls back to the D3D compose) for A/B testing and emergencies.
- [ ] Soak: re-run a multi-hour session; the crash needed ~166k frames plus
  layer-copy churn to reproduce.
- [ ] Ask user: "image working?"

## Slice 5b — Retire the D3D video path (Vulkan-only)
- [x] D3D11 SBS compose deleted (`ComposeSBSLayer`, the blit VS/PS, sampler,
  blend states, input layout, vertex buffer, bounds CB and the conversion RTV).
  A Vulkan compose failure now drops that frame; there is no second renderer.
- [x] D3D11 staging readback deleted. `ReadbackToBuffer` hands the encoder the
  Vulkan NV12 buffer, or the Vulkan host copy when the compute pass did not run.
- [x] The second D3D11 device is gone: the encoder uses the compositor device
  only to open the eye textures the composer imports.
- [x] `VulkanInterop` deleted. `VulkanCompose` owns the native target, the
  compose pass, the NV12 compute pass and the host-visible probe copy
  (`ProbeCopy`/`ProbeData`), so the encoder creates no D3D11 objects at all and
  the conversion RT plus `CopyToRt` are gone.
- [x] Verified live: `Vulkan conversion pipeline ready`, compose streaming,
  preview 60 fps / 0 drops, no driver-side errors.

## Slice 6 — Encoder backend: NVENC via Vulkan, no D3D11VA
- [ ] Replace `AV_HWDEVICE_TYPE_D3D11VA` (AMF) with
  `AV_HWDEVICE_TYPE_CUDA` (nvenc, wrapped with the same Vulkan device via
  CUDA↔Vulkan interop) or `VAAPI` on Linux. Candidate list becomes
  `{ h264_nvenc, h264_vaapi, libx264 }` when `AV_DRIVER_VULKAN` is on.
- [ ] Verify GPU encoding path still feeds Annex-B + SPS/PPS to the phone
  (`H264Utils` unchanged).
- [ ] Ask user: "image working?"

## Slice 7 — Linux build (scaffolding)
- [x] `CMakeLists.txt` targets the Linux shared lib and lists only the portable
  sources; `DirectMode.cpp`, `DxbcToSpv.cpp` and `include/dxil-spirv/` are
  Windows-only and stay out.
- [x] `scripts/compile-driver-linux.sh` + `scripts/install-driver-linux.sh`
  configure, build and install into the per-user SteamVR driver directory.
- [x] `include/Platform.h` shim: POSIX sockets, `shm_open`+`mmap` bridge regions,
  steady-clock timing, `dlopen`, portable formatting/parsing. No Winsock, CRT
  `_s` calls or `GetEnvironmentVariableA` left outside the shim.
- [x] Shaders compile at build time: the HLSL lives once in
  `include/shaders/*.hlsli`, CMake extracts it, runs `dxc -spirv` and embeds the
  SPIR-V (`cmake/EmbedSpirv.cmake`). Windows keeps its runtime
  `D3DCompile`+`DxbcToSpv` path.
- [ ] Compile check on a Linux host. `docs/LINUX_PORT.md` lists the expected
  failures, starting with the descriptor-slot mismatch between `dxc` and
  `dxbc-spirv`.

## Slice 8 — Linux DirectMode: Vulkan-native swap textures
- [x] `src/DirectModePosix.cpp` implements `CreateSwapTextureSet` with Vulkan
  images on device-local memory, exported as dma-buf fds
  (`vkGetMemoryFdKHR`) and published as the shared texture handles.
- [x] `AcquireSyncTexture` imports the compositor's sync handle as an external
  semaphore (`vkImportSemaphoreFdKHR`) and waits on it; no DXGI mutexes on Linux.
- [x] `GpuImage` (handle + extent + format) replaces `ID3D11Texture2D*` across
  DirectMode → encode queue → composer, so `VulkanCompose::FindOrImportEye`
  imports a Windows NT handle or a Linux dma-buf through one code path.
- [x] `BridgeServer` uses `shm_open`+`mmap` on Linux with the same
  `BridgeProtocol.h` layout; region names are `/cardboard_pp_bridge{,_cmd}`.
- [ ] Verify on Linux SteamVR: align the descriptor slots (see
  `docs/LINUX_PORT.md`), confirm the compositor's swap-set handle convention,
  then check image on the phone and pose tracking.

## Slice 9 — Cleanup and build scripts
- [ ] Delete legacy D3D11 paths that are no longer reachable (behind flags
  now).
- [ ] New install script for Linux driver: `scripts/install-driver-linux.sh`.
- [ ] Update README/docs to note the Vulkan requirement (SteamVR 2.5.1+,
  Vulkan 1.2+).

## Slice 10 — Bridge on Linux (after driver-only pipeline works)
- [ ] `bridge-shm` SHM → `shm_open`/`mmap` (same protocol.rs layout).
- [ ] `ffmpeg` preview decode already portable; check `zune-jpeg`, sockets.
- [ ] Slint UI runs on Linux (Wayland/X11).
- [ ] `scripts/compile-bridge.sh`.
- This slice: **only after** driver-works-on-Linux is confirmed by the user.

---

## Non-negotiables carried over
- `CardboardWire.h` and `protocol.rs`/`BridgeProtocol.h` contracts untouched.
- Video path stays UDP 42069 direct driver→phone; bridge never touches video.
- No mock frameworks; tests updated when wire-handling code changes.
- Every slice ends with a driver-only build + SteamVR restart + user image
  check before the next slice starts.