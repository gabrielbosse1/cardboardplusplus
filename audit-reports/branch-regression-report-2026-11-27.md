# Branch Regression & Cross-Platform Impact Report (2026-11-27)

- **Audited branch:** `arena/1b11b2be-cardboardplusplus` (branched from `arena/1ac5e66b-cardboardplusplus` @ `615803f82818ef279f2fa3224e8bbc4d0a9e62a2`)
- **Upstream base (`master`):** `51fca6af3cf174db4c24e16a91a4b7f9a31f770` (Merge PR #7 `linux-port`)
- **Known-good baseline:** Prior to the commits on `51fca6a..615803f`, the Linux driver and bridge ran end-to-end on real hardware (CachyOS x86_64, GCC 16.2.1, Mesa RADV on AMD RX 6600M, SteamVR 2.17+, 60 fps stream with 0 drops; see `audit-reports/linux-port-test-report.md` and commits `afc3f35` / `b56230c`). Meanwhile, merging `linux-port` (`51fca6a`) into `master` introduced Windows build breakages (`PlatformSockLen` missing on `_WIN32`, `Debug|x64` CRT/STL mismatch with `dxbc-spirv.lib`, and `v145` toolset assumptions).

---

## 1. Executive Summary

Auditing every commit between `master` (`51fca6a`) and `615803f` reveals:

1. **Two critical Linux runtime regressions that break working Linux video streaming:**
   - **P1 (`694c3b9`, `DirectModePosix.cpp`):** `SubmitLayer` stopped clearing `m_submitLayers` and added a 4-layer cap that returns **before** setting `m_hasSubmit = true`, while `Present` was left copying `m_submitLayers` without draining/clearing it. **Result:** Frames 1–4 composite stale previous frames on top of each other, and **from Frame 5 onward (`~60 ms` into streaming), 100% of frames are dropped permanently on every Linux machine.**
   - **P2 (`ef578e7`, `DirectModePosix.cpp` + `VulkanCompose.cpp`):** `AcquireSyncTexture` was replaced with an unconditional `return false;` while `Present` aborts whenever `syncTexture != 0`. **Result:** Whenever SteamVR's Linux compositor supplies a non-zero `syncTexture` handle, **100% of frames are dropped from Frame 1.** Furthermore, the underlying Linux DMA-BUF fence wait in `VulkanCompose::Compose` waited at `VK_PIPELINE_STAGE_TRANSFER_BIT` instead of `VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT`, only synchronized layer `0` (ignoring layers `1..3`), and leaked imported semaphores/sync-file FDs if `vkQueueSubmit` failed.
2. **One high-severity Linux linker/loader regression on non-Debian distros (Arch/CachyOS, Fedora, openSUSE):**
   - **P3 (`a355aba` + `ef8fdcc`, `CMakeLists.txt` & `scripts/install-driver-linux.sh`):** Linking SteamVR's unprefixed `openvr_api.so` via `-Wl,-l:openvr_api.so` without `--as-needed` silently passes on Ubuntu CI (where GCC defaults to `--as-needed`), because the driver references zero symbols from `openvr_api.so`. On Arch/CachyOS, Fedora, and openSUSE (where GCC/binutils defaults to `--no-as-needed`), the linker records `DT_NEEDED: openvr_api.so` in `driver_cardboardplusplus.so`. Because `SteamVR/bin/linux64` is outside `BUILD_RPATH` inside the Steam Linux Runtime and `install-driver-linux.sh` only excluded `libopenvr_api.so` (not `openvr_api.so`), `vrserver`'s `dlopen()` can fail at load time on machines without `/usr/lib/libopenvr_api.so`.
3. **Three medium-severity Windows Debug & local build regressions/caveats:**
   - **P4a (`a355aba`, `driver_cardboardplusplus.vcxproj`):** Setting `<UseDebugLibraries>false</UseDebugLibraries>` on `Debug|x64` without specifying `<Optimization>Disabled</Optimization>` causes MSBuild (`Microsoft.Cpp.Default.props`) to compile `Debug|x64` with **`/O2` (`MaxSpeed`) optimization**, breaking step-through debugging and optimizing away local variables.
   - **P4b (`fc9321c`, `driver_cardboardplusplus.vcxproj` + `DirectMode.cpp`):** Removing `_DEBUG` from `Debug|x64` (needed to match the release STL/CRT ABI of vendored `dxbc-spirv.lib`) silently disabled `#ifdef _DEBUG` → `#define DRIVER_DIAG` in `DirectMode.cpp`, turning off `Present` FPS, skip counters, and 120-frame diagnostic logs in Windows Debug builds.
   - **P4c (`linux-port` carry-over in `driver_cardboardplusplus.vcxproj` + `scripts/compile-driver.ps1`):** All `.vcxproj` configurations hardcode `<PlatformToolset>v145</PlatformToolset>` (VS 2026). GitHub Actions overrides this with `/p:PlatformToolset=v143`, but `scripts/compile-driver.ps1` did not — breaking local Windows builds on machines with Visual Studio 2022 (`v143`) when `v145` is not installed.
4. **Several genuine fixes on `51fca6a..615803f` are sound and worth keeping:**
   - Defining `PlatformSockLen = int` under `#ifdef _WIN32` in `Platform.h` (`b824f48`);
   - Stopping `VulkanCompose::ImportEyeSemaphores` from `close()`-ing `sync_file` FDs already consumed by `vkImportSemaphoreFdKHR` (`ef578e7` F03 hunk);
   - Returning `bool` from `VulkanCompose::CopyToHost()` so failed host readbacks never feed stale `m_hostMap` pixels to `swscale` (`c190d5e` F20 hunk);
   - Preserving `oldW`/`oldH` across `ApplyHardwareCap` → `ApplyEncoderSettings` so resolution-only cap reductions actually rebuild the encoder (`c190d5e` F05 hunk);
   - Guarding `SetupNv12()` with `VulkanContext::QueueSupportsCompute()` (`329c3b9` F19 hunk);
   - Releasing `MediaCodec` when `configure()`/`start()` throws in Android `VideoDecoder.configureLocked()` (`c190d5e` F21 hunk);
   - Honoring `m_streamEnabled` in Linux `Present()` (`694c3b9` F11 hunk).

---

## 2. Commit-by-Commit Disposition (`51fca6a..615803f`)

| Commit | Summary | Linux Impact | Windows Impact | Verdict |
|---|---|---|---|---|
| `cbe14de` | `docs: add compatibility audit 2026-11-27` | None (docs) | None (docs) | Keep |
| `ef578e7` | `fix(linux): reject unknown sync handles and retain imported fd ownership (F02, F03)` | **BREAKS LINUX** when `syncTexture != 0` (`AcquireSyncTexture` always returns `false`); `VulkanCompose.cpp` FD ownership fix (F03) is **GOOD** | None (`#ifndef _WIN32`) | **Fix `AcquireSyncTexture` + Vulkan eye fence stage/cleanup; keep FD ownership fix** |
| `c190d5e` | `fix(video): enforce copy success and release failed decoder (F05, F12, F20, F21)` | Safe; fixes `CARDBOARD_CAP` resolution rebuild and stale `CopyToHost` buffer | Safe; same benefits on Windows + Android `MediaCodec` leak fix | Keep |
| `694c3b9` | `fix(linux): preserve submitted layers and honor stream setting (F10, F11)` | **BREAKS 100% OF LINUX STREAMS** after 4 submitted layers (`m_submitLayers` never cleared in `Present`); `m_streamEnabled` check (F11) is safe | None (Linux-only file) | **Fix `Present` layer drain + skipped-present clear; keep `m_streamEnabled` gate** |
| `ed07d9c` | `fix(diagnostics): log Vulkan identity and clarify encoder choices (F08, F17)` | Safe (one startup log line + UI hint string) | Safe | Keep |
| `329c3b9` | `fix(video): guard compute queue and signal SDR matrix (F14, F19)` | Safe; `QueueSupportsCompute()` is `true` on normal GPUs and safely falls back to `swscale` otherwise; BT.601 limited VUI matches shader | Safe; accepted by AMF, NVENC, QSV, and `libx264` | Keep (see color-space note in §3.5) |
| `ac169d8` | `fix(ci): repair Linux workflow and expose Windows build errors` | CI-only | CI-only (surfaces MSBuild errors in annotations) | Keep |
| `b824f48` | `fix(build): define Windows socket length and repair Linux CI stub` | Safe (`socklen_t` unchanged on POSIX) | **FIXES WINDOWS BUILD** (`PlatformSockLen` was missing under `_WIN32` on `master`) | Keep |
| `fc9321c` | `fix(build): align Windows debug runtime and Linux CI dependencies` | CI-only | Fixes `Debug|x64` CRT mismatch, **but disables `DRIVER_DIAG`** by dropping `_DEBUG` | **Restore `DRIVER_DIAG` in `Debug|x64`** |
| `a355aba` | `fix(build): locate SteamVR library and align debug CRT` | Finds unprefixed `openvr_api.so` | Fixes `Debug|x64` link, **but `<UseDebugLibraries>false` turns on `/O2` in Debug** | **Force `<Optimization>Disabled</Optimization>` in `Debug|x64`** |
| `4c7a299` | `fix(ci): bound Linux build parallelism and retain failure details` | CI-only | None | Keep |
| `ef8fdcc` | `fix(linux): link unprefixed SteamVR library by exact name` | **BREAKS `--no-as-needed` DISTROS** (Arch/CachyOS/Fedora/openSUSE) when `/usr/lib/libopenvr_api.so` is absent | None (CMake is Linux-only) | **Wrap link in `--as-needed` + update `install-driver-linux.sh` + CI ELF check** |
| `615803f` | `Delete PR_BODY.md` | None | None | Keep |

---

## 3. Detailed Problem Analysis

### 3.1 Problem 1 (CRITICAL — Linux): `m_submitLayers` Never Drained in `Present()`, Freezing Video After 4 Layers

- **Location:** `driver_cardboardplusplus/src/DirectModePosix.cpp` — `HmdDriver::SubmitLayer` (lines 270–290) and `HmdDriver::Present` (lines 297–333)
- **Introduced in:** `694c3b97237e686539a00a9dac28de7e3edd4deb` (`fix(linux): preserve submitted layers and honor stream setting (F10, F11)`)

#### Code on `master` (`51fca6a`, working single-layer Linux baseline)
In `51fca6a`, `SubmitLayer` cleared `m_submitLayers` on each call, while `Present` copied `m_submitLayers` by value without clearing it:
```cpp
void HmdDriver::SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2])
{
    std::lock_guard<std::mutex> lock(m_submitLayersMutex);
    m_submitLayers.clear();
    ...
    m_submitLayers.push_back(info);
    m_hasSubmit.store(true, std::memory_order_release);
}
```
```cpp
    std::vector<SubmitLayerInfo> layers;
    {
        std::lock_guard<std::mutex> lock(m_submitLayersMutex);
        if (!m_hasSubmit.load(std::memory_order_acquire)) {
            if (hasSync) ReleaseSyncTexture();
            return;
        }
        layers = m_submitLayers;
        m_hasSubmit.store(false, std::memory_order_release);
    }
```

#### What `694c3b9` changed
To allow multi-layer composition (e.g. base VR scene + SteamVR dashboard overlay, up to `VulkanCompose::kMaxLayers = 4`), `694c3b9` replaced `m_submitLayers.clear()` in `SubmitLayer` with:
```cpp
    if (m_submitLayers.size() >= 4) {
        static unsigned droppedLayers = 0;
        if (++droppedLayers <= 3 || droppedLayers % 300 == 0)
            DriverLog("DirectMode: layer limit reached (drop=%u)", droppedLayers);
        return;
    }
```
**However, `Present()` was never updated to clear `m_submitLayers` when consuming a frame (nor when dropping a frame on early return).**

#### Frame-by-frame failure trace on Linux
1. **Frame 1:** `SubmitLayer` pushes `[L1]` (`size == 1`), sets `m_hasSubmit = true`. `Present` copies `layers = [L1]`, sets `m_hasSubmit = false`, and **leaves `m_submitLayers == [L1]`**.
2. **Frame 2:** `SubmitLayer` pushes `[L2]` (`size == 2`), sets `m_hasSubmit = true`. `Present` composites **`[L1, L2]`** — blending Frame 2 over Frame 1's swapchain texture (which `vrcompositor` may already be overwriting!).
3. **Frame 3:** `m_submitLayers` grows to `[L1, L2, L3]` (`size == 3`).
4. **Frame 4:** `m_submitLayers` grows to `[L1, L2, L3, L4]` (`size == 4`).
5. **Frame 5 and every frame forever after (~66 ms at 60 Hz):**
   - `SubmitLayer` sees `m_submitLayers.size() >= 4` and **returns before `m_hasSubmit.store(true)`**.
   - `m_hasSubmit` stays `false` permanently.
   - `Present` checks `if (!m_hasSubmit.load(std::memory_order_acquire))` and **returns early without encoding on 100% of subsequent frames**.
6. **Secondary wedge during encoder reconfiguration (`Present` lines 309–320):**
   - Even if normal `Present` drained `m_submitLayers`, lines 309–320 return early when `!m_vulkan.IsReady() || !m_encoderInitialized || !m_pVideoEncoder` (which happens whenever `ApplyEncoderSettings` / `ApplyHardwareCap` temporarily tears down and recreates the encoder!) and only set `m_hasSubmit = false` without clearing `m_submitLayers`. Four compositor ticks (~66 ms) during encoder re-init fill `m_submitLayers` to 4 and permanently wedge `SubmitLayer` once the new encoder comes online.

#### Why CI missed it
`linux-driver.yml` compiles `libdriver_cardboardplusplus.so`, checks exported/undefined ELF symbols with `nm -D`, and runs `test_wire_protocol.cpp` — which had no test covering the `SubmitLayer` / `Present` layer-queue lifecycle.

#### Fix
1. In `DirectModePosix.cpp::Present`, atomically swap and clear `m_submitLayers` under `m_submitLayersMutex` (matching Windows `DirectMode.cpp:263–265`):
   ```cpp
   layers.swap(m_submitLayers);
   m_submitLayers.clear();
   m_hasSubmit.store(false, std::memory_order_release);
   ```
2. Clear `m_submitLayers` under `m_submitLayersMutex` on the early-return paths in `Present` (`!m_vulkan.IsReady() || !m_encoderInitialized || !m_pVideoEncoder` and `!AcquireSyncTexture`), so skipped frames never carry stale layers into future frames.
3. Add deterministic state-machine tests in `driver_cardboardplusplus/tests/test_wire_protocol.cpp` covering 100+ consecutive frames, 4-layer scenes+overlays, >4-layer per-frame capping, and encoder-reinit / stream-disabled recovery.

---

### 3.2 Problem 2 (CRITICAL — Linux): `AcquireSyncTexture` Unconditionally Drops All Non-Zero `syncTexture` Frames, Plus Eye DMA-BUF Fence Stage & Leak Defects

- **Locations:**
  - `driver_cardboardplusplus/src/DirectModePosix.cpp` — `Present` (lines 316–320), `AcquireSyncTexture` (lines 395–405)
  - `driver_cardboardplusplus/src/VulkanCompose.cpp` — `ImportEyeSemaphores` (lines 34–65), `Compose` (lines 1196–1247)
- **Introduced / Exposed in:** `ef578e7a28a32e6e1db308d8538a56b69d350749` (`fix(linux): reject unknown sync handles and retain imported fd ownership (F02, F03)`) and `afc3f35`

#### Analysis of `DirectModePosix.cpp::AcquireSyncTexture`
1. **Why Windows needs `AcquireSyncTexture` in `Present()`:**
   On Windows (`DirectMode.cpp`), eye swapchain textures are `D3D11_RESOURCE_MISC_SHARED` (non-keyed), and `Present()` immediately runs `m_pHostContext->CopySubresourceRegion(...)` on the CPU thread. SteamVR's Windows D3D11 compositor passes a `D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX` `syncTexture` handle so the driver acquires key `0` before copying on D3D11.
2. **How Linux DirectMode actually works:**
   - On Linux (`DirectModePosix.cpp`), swapchain eye images are allocated through `vr::IPCResourceManager()->NewSharedVulkanImage(...)`, and all `SharedTextureHandle_t` values in SteamVR-for-Linux are **opaque 64-bit IPC resource handles** (e.g. `0x70000abb3`; see `ValveSoftware/openvr#1884` and `CreateSharedEye`), **not** POSIX file descriptors.
   - `DirectModePosix.cpp::Present` performs **zero** GPU or CPU reads of the eye images — it only resolves which swapchain slot was submitted, puts `set->fds[idxLeft]` / `setRight->fds[idxRight]` into `m_pendingFrame`, and wakes the encode thread.
   - On the encode thread, `VulkanCompose::Compose` exports the Linux kernel's implicit `dma_resv` write fence directly from each eye's DMA-BUF file descriptor via `ioctl(fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, DMA_BUF_SYNC_READ)`, imports each `sync_file` as a temporary binary `VkSemaphore` (`VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT | VK_SEMAPHORE_IMPORT_TEMPORARY_BIT`), and waits on those semaphores in `vkQueueSubmit`.
3. **Why both `b56230c` (`master`) and `ef578e7` (`HEAD`) were broken when `syncTexture != 0`:**
   - **`b56230c` (`master`):** Cast `(int)syncTexture` as if it were a raw fd; chained `VkImportSemaphoreFdInfoKHR` onto `VkSemaphoreCreateInfo::pNext` (which `vkCreateSemaphore` ignores — `VkImportSemaphoreFdInfoKHR` is only consumed by `vkImportSemaphoreFdKHR`, leaking the `dup()`ed fd); and called `vkWaitSemaphores` with `pValues = nullptr` on a binary semaphore (`VUID-VkSemaphoreWaitInfo-pSemaphores-03256` violation — Mesa's `vk_common_WaitSemaphores` dereferences `pWaitInfo->pValues[i]` unconditionally, causing an immediate `SIGSEGV` in `vrserver`!). On `master`, this was dormant only when `vrcompositor` passed `syncTexture == 0`.
   - **`ef578e7` (`HEAD`):** Replaced `AcquireSyncTexture` with `return false;`, while `Present` kept `if (hasSync && !AcquireSyncTexture(syncTexture)) return;` — turning any non-zero `syncTexture` from SteamVR into a **100% frame drop / black screen**.

#### Analysis of `VulkanCompose.cpp` Linux Eye Semaphore Path
While commit `ef578e7` **correctly** fixed a real FD double-close bug in `ImportEyeSemaphores` (removing `::close(fd)` on `sync_file` FDs whose ownership was already transferred to Vulkan on `VK_SUCCESS`), inspecting the surrounding Linux sync block in `VulkanCompose.cpp:1196–1247` reveals three remaining defects:
1. **Wrong pipeline wait stage (`VulkanCompose.cpp:1206`):**
   `waitStages[s]` was set to `VK_PIPELINE_STAGE_TRANSFER_BIT`. However, `VulkanCompose::Compose` does **not** copy eye images with a transfer command — it binds them as `COMBINED_IMAGE_SAMPLER` descriptors and samples them inside the graphics render pass fragment shader (`VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT`, matching the initial layout barrier at line 1115). In Vulkan, `VK_PIPELINE_STAGE_TRANSFER_BIT` in `pWaitDstStageMask` does **not** block `VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT`, allowing the fragment shader to sample an eye image before the compositor's GPU write fence signals.
2. **Only layer `0` was synchronized (`VulkanCompose.cpp:1201`):**
   `ImportEyeSemaphores` only exported `sync_file` fences for `lefts[0]` and `rights[0]`. With multi-layer composition enabled (`n` up to `kMaxLayers = 4`), layers `1..n-1` (overlays/dashboard) had no fence wait.
3. **Semaphore + FD leak on `vkQueueSubmit` failure (`VulkanCompose.cpp:1224–1233`):**
   If `submitQ` returned an error, `Compose` returned `false` at line 1232 without destroying `eyeSems`, leaking both the `VkSemaphore` objects and the imported `sync_file` FDs attached to them.

#### Fix
1. **In `DirectModePosix.cpp`:**
   - Have `AcquireSyncTexture` log once when a non-zero `syncTexture` handle is observed (so logs clearly record whether a given SteamVR build supplies one without flooding SteamVR's IPC log pipe), mark `m_syncAcquired = true`, and return `true` so frames proceed to `VulkanCompose::Compose` where the kernel DMA-BUF write fences synchronize GPU access.
   - Remove the unused `kSemaphoreTimeoutNs` constant and unused `m_syncSemaphore` path so compilation stays warning-free under `-Wall -Wextra`.
2. **In `VulkanCompose.cpp`:**
   - Export and import `sync_file` semaphores across **all `n` submitted layers** (`lefts[0..n-1]`, `rights[0..n-1]`, up to `2 * kMaxLayers`), deduplicating identical FDs so we do not redundantly export/wait twice if two layers reference the same fd.
   - Set `waitStages[s] = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT` so fragment-shader sampling in the compose render pass waits on the compositor's DMA-BUF write fences.
   - Destroy `eyeSems` on both the `vkQueueSubmit` error path and the post-fence path (and on `waitFences` timeout/failure before `Fail()` disables compose, when safe, or when submission itself never queued). Note: if `submitQ` failed, the GPU never queued the semaphore wait so `vkDestroySemaphore` is immediately legal per Vulkan spec; if `submitQ` succeeded and `waitFences` succeeded, the batch completed so `vkDestroySemaphore` is also legal.

---

### 3.3 Problem 3 (HIGH — Linux Distro Portability): Unprefixed `openvr_api.so` Link Emits Unresolvable `DT_NEEDED` on `--no-as-needed` Toolchains (Arch / CachyOS / Fedora / openSUSE)

- **Locations:**
  - `driver_cardboardplusplus/CMakeLists.txt` (lines 33–53, 147–167)
  - `scripts/install-driver-linux.sh` (lines 43–50)
  - `.github/workflows/linux-driver.yml` (lines 83–104)
- **Introduced in:** `a355aba56ae9d023c5cc1389212e8f5a74632142` and `ef8fdccb43b4c4601ecfcaa9e23dd5e622396998`

#### Root Cause
1. SteamVR server drivers (`driver_*.so` loaded into `vrserver`) use **zero** exported functions from `openvr_api.so`; every OpenVR driver call (`VRServerDriverHost()`, `VRProperties()`, `VRDriverLog()`, `VRDriverContext()`, `IPCResourceManager()`) goes through inline vtable thunks in `openvr_driver.h`. (`openvr_api.so` is the client SDK library for VR applications.)
2. Commits `a355aba` and `ef8fdcc` added `find_file(OPENVR_API_LIB NAMES openvr_api.so ...)` and linked `driver_cardboardplusplus` with:
   ```cmake
   target_link_directories(driver_cardboardplusplus PRIVATE "${CBPP_OPENVR_LIBDIR}")
   set(CBPP_OPENVR_LINK "-Wl,-l:openvr_api.so")
   ```
3. **Why Ubuntu CI passed while Arch/CachyOS/Fedora/openSUSE can break:**
   - Debian/Ubuntu patches GCC's driver specs to pass `--as-needed` to `ld` by default. Since `driver_cardboardplusplus.so` references zero symbols from `openvr_api.so`, Ubuntu `ld` silently discards `openvr_api.so` and emits **no** `DT_NEEDED` tag.
   - Upstream GCC/binutils — used by **Arch Linux, CachyOS, Manjaro, Fedora, RHEL, and openSUSE** — defaults to **`--no-as-needed`**. On those distros:
     - If `/usr/lib/libopenvr_api.so` is not installed (`scripts/install-linux.sh` does **not** install any `openvr` distro package), CMake falls through to `SteamVR/bin/linux64/openvr_api.so` and emits `DT_NEEDED: [openvr_api.so]` (or `[libopenvr_api.so]`) into `driver_cardboardplusplus.so`.
     - `CMakeLists.txt` overrides `BUILD_RPATH` to `"$ORIGIN:/usr/lib:/usr/lib64:/run/host/usr/lib:/run/host/usr/lib64"`, which **does not** contain `SteamVR/bin/linux64`.
     - In glibc, `vrserver`'s own `DT_RUNPATH` is **not** inherited by transitive `DT_NEEDED` entries of `dlopen()`-loaded plugins. Inside Steam Linux Runtime, if `LD_LIBRARY_PATH` does not contain `SteamVR/bin/linux64`, `dlopen("driver_cardboardplusplus.so")` fails with `openvr_api.so: cannot open shared object file: No such file or directory`.
   - Separately, in `scripts/install-driver-linux.sh` line 48:
     ```bash
     ld-linux*|linux-vdso*|libopenvr_api.so|libvulkan.so.1|libX11.so.6) continue ;;
     ```
     The skip pattern only matched `libopenvr_api.so`, not unprefixed `openvr_api.so`.

#### Fix
1. In `driver_cardboardplusplus/CMakeLists.txt`, wrap `${CBPP_OPENVR_LINK}` in `-Wl,--push-state,--as-needed` ... `-Wl,--pop-state` so **all** ELF linkers (regardless of distro defaults) drop the unused `openvr_api` `DT_NEEDED` entry while still verifying link flags cleanly.
2. In `scripts/install-driver-linux.sh`, skip both `libopenvr_api.so` and `openvr_api.so` in `stage_closure`.
3. In `.github/workflows/linux-driver.yml`, test linking and verify via `readelf -d` that `libdriver_cardboardplusplus.so` carries **no** `NEEDED` dependency on `openvr_api.so` or `libopenvr_api.so`.

---

### 3.4 Problem 4 (MEDIUM — Windows): `Debug|x64` Compiles with `/O2`, Loses `DRIVER_DIAG`, and `compile-driver.ps1` Fails on VS 2022 (`v143`) Without `v145`

- **Locations:**
  - `driver_cardboardplusplus/driver_cardboardplusplus.vcxproj` (lines 94–99, 180–192)
  - `driver_cardboardplusplus/src/DirectMode.cpp` (lines 42–44)
  - `scripts/compile-driver.ps1` (lines 20–30)
- **Introduced in:** `fc9321c3a27655003f1c1533a07782db9b9008b4`, `a355aba56ae9d023c5cc1389212e8f5a74632142`, and `linux-port` merge (`51fca6a`)

#### Root Cause
1. **Why `Debug|x64` failed to link after `linux-port` (`51fca6a`):**
   `linux-port` vendored prebuilt release static libraries `lib/dxil-spirv/dxbc-spirv.lib` and `lib/dxil-spirv/dxil-spirv-sbv.lib` (compiled with `/MD` `MultiThreadedDLL` and `_ITERATOR_DEBUG_LEVEL=0`). Building `driver_cardboardplusplus` in `Debug|x64` with `/MDd` and `_ITERATOR_DEBUG_LEVEL=2` failed with MSVC linker error `LNK2038`.
2. **Side effect A — `Debug|x64` silently became `/O2` optimized (`a355aba`):**
   Setting `<UseDebugLibraries>false</UseDebugLibraries>` in `PropertyGroup` without setting `<Optimization>Disabled</Optimization>` in `Debug|x64`'s `<ClCompile>` causes MSBuild's `Microsoft.Cpp.props` to apply the non-debug default `<Optimization>MaxSpeed</Optimization>` (`/O2`), optimizing away local variables and scrambling line stepping in Visual Studio's debugger.
3. **Side effect B — `DRIVER_DIAG` silently turned off in `Debug|x64` (`fc9321c`):**
   `DirectMode.cpp:42–44` had:
   ```cpp
   #ifdef _DEBUG
   #define DRIVER_DIAG
   #endif
   ```
   Removing `_DEBUG` from `Debug|x64` (necessary because MSVC's `<yvals_core.h>` rejects `_DEBUG` combined with `/MD` or `_ITERATOR_DEBUG_LEVEL=0`) disabled `DRIVER_DIAG` in `Debug|x64`, suppressing `Present` FPS logs, `Present SKIPPED` diagnostics, and `[DIAG Present]` 120-frame telemetry on Windows Debug builds.
4. **Side effect C — Hardcoded `<PlatformToolset>v145</PlatformToolset>` breaks `scripts/compile-driver.ps1` on VS 2022 (`v143`):**
   `.vcxproj` sets `<PlatformToolset>v145</PlatformToolset>` (Visual Studio 2026). CI workflows (`build-driver.yml` and `release.yml`) explicitly pass `/p:PlatformToolset=v143`, whereas `scripts/compile-driver.ps1` invoked `msbuild` with no toolset override — failing with `MSB8020: The build tools for v145 cannot be found` on any developer machine that has Visual Studio 2022 (`v143`) installed rather than VS 2026.

#### Fix
1. In `driver_cardboardplusplus/driver_cardboardplusplus.vcxproj` (`Debug|x64` `<ClCompile>`):
   - Add `<Optimization>Disabled</Optimization>` so `Debug|x64` compiles with `/Od` (unoptimized, full debug symbols) while keeping `RuntimeLibrary=MultiThreadedDLL` and `_ITERATOR_DEBUG_LEVEL=0` for ABI compatibility with `dxbc-spirv.lib`.
   - Add `DRIVER_DIAG` to `Debug|x64`'s `<PreprocessorDefinitions>`, and guard `#ifndef DRIVER_DIAG` around `#define DRIVER_DIAG` in `DirectMode.cpp`.
2. In `scripts/compile-driver.ps1`:
   - Detect whether the located Visual Studio installation includes the `v145` MSBuild platform toolset; if `v145` is absent and `v143` is installed, automatically pass `/p:PlatformToolset=v143` to `msbuild` (matching CI while still honoring `v145` on VS 2026 machines).

---

### 3.5 Behavioral & Cross-Branch Notes (Non-Breaking, Documented for Awareness)

1. **H.264 VUI Color Metadata (`329c3b9`, `VideoEncoderFFmpeg.cpp:44–45`):**
   - Sets `m_pCodecContext->color_range = AVCOL_RANGE_MPEG` and `m_pCodecContext->colorspace = AVCOL_SPC_SMPTE170M` (BT.601 limited range).
   - Verified compatible with all 5 configured encoders (`h264_amf`, `h264_nvenc`, `h264_qsv`, `h264_vaapi`, `libx264`), with `nv12_cs.hlsli` (`16 + 65.481 R + 128.553 G + 24.966 B`), with `sws_scale`'s default matrix, and with Android's `H264NalParser.parseSpsDimensions` (which stops right after `frame_cropping_flag`, prior to `vui_parameters_present_flag`).
   - Note: Decoders that previously guessed BT.709 for untagged >=720p streams will now use BT.601 as signaled, matching the shader's actual coefficients.
2. **Files Overlapping Other Open Work (e.g. PR #9):**
   - `cardboardplusplus-android/.../video/VideoDecoder.java`, `driver_cardboardplusplus/src/EncoderSetup.cpp`, `driver_cardboardplusplus/src/VideoEncoderFFmpeg.cpp`, and `bridge/crates/cardboard-bridge/ui/app.slint` are touched in small ways on this branch (`8`, `12`, `3`, and `2` lines respectively); expect minor rebase/merge resolution when combining with protocol/UI branches.

---

## 4. Fix Execution Plan

Each fix is applied in its own focused commit, verified locally (C++ syntax/warnings check + expanded `test_wire_protocol` suite), and validated across all GitHub Actions workflows on Windows and Linux:

1. **Commit 1 (Report):** Add this report (`audit-reports/branch-regression-report-2026-11-27.md`).
2. **Commit 2 (Fix P1 — Linux layer queue wedge):** Drain `m_submitLayers` in `DirectModePosix.cpp::Present` (both normal consumption and skipped-present early returns) and add regression tests in `driver_cardboardplusplus/tests/test_wire_protocol.cpp`.
3. **Commit 3 (Fix P2 — Linux sync handle & Vulkan eye fence stage/cleanup):** Update `DirectModePosix.cpp::AcquireSyncTexture` so non-zero compositor sync handles do not drop frames, synchronize all submitted layers at `VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT` in `VulkanCompose.cpp`, and clean up imported semaphores on `vkQueueSubmit` failure.
4. **Commit 4 (Fix P3 — Linux `--as-needed` OpenVR link & installer staging):** Wrap `CBPP_OPENVR_LINK` with `--push-state,--as-needed` in `CMakeLists.txt`, add `openvr_api.so` to `scripts/install-driver-linux.sh`'s skip pattern, and assert zero `openvr_api` `DT_NEEDED` entries in `.github/workflows/linux-driver.yml`.
5. **Commit 5 (Fix P4 — Windows `Debug|x64` `/Od` + `DRIVER_DIAG` & `compile-driver.ps1` toolset fallback):** Restore `/Od` and `DRIVER_DIAG` in `Debug|x64` (`driver_cardboardplusplus.vcxproj` + `DirectMode.cpp`), add automatic `v143` fallback in `scripts/compile-driver.ps1`, and update `audit-reports/fix-worklog.md`.
