# Linux port status and handoff

Windows is the verified reference: `scripts/compile-driver.ps1` builds it, it runs live
(60 fps compose, 0 drops, no driver errors). The Linux code in this tree has **never been
compiled** — there was no Linux host while it was written. Treat every Linux-only file as
a reviewed first draft, not as working code.

## Build and run (Linux)

```bash
sudo apt install build-essential cmake pkg-config libavcodec-dev libavformat-dev \
     libavutil-dev libswscale-dev libvulkan-dev libx11-dev spirv-tools
# dxc: grab a DirectXShaderCompiler Linux release and put `dxc` on PATH
scripts/compile-driver-linux.sh
scripts/install-driver-linux.sh
"$OPENVR_DIR/bin/linux64/vrserver.sh" -vrdriver cardboardplusplus
```

Logs: `$HOME/.local/share/Steam/logs/driver.log`.
`CARDBOARD_DEBUG=1` enables the verbose lines; before the SteamVR log host exists, driver
lines fall back to stderr.

## What is already ported

| Area | Change | Status |
|---|---|---|
| Sockets | `include/Platform.h` wraps Winsock vs POSIX: `PlatformSocket`, `PlatformCloseSocket`, `PlatformWouldBlock`, `PlatformTimedOut`, `PlatformSetRecvTimeout`, `PlatformSetNonBlocking` | Windows build + live verified |
| Timing / env | `PlatformNowMs/PlatformNowUs` (steady_clock), `PlatformGetEnv`, `PlatformDebugEnv` | verified |
| Logging | `snprintf` everywhere, stderr fallback when `VRDriverLog()` is null | verified |
| Parsing | `PlatformParseInts` / `PlatformParseWord` replace `sscanf_s` | verified |
| Bridge SHM | `PlatformCreateMapping/PlatformOpenMapping/PlatformMapView/PlatformUnmapView` — `CreateFileMappingW` vs `shm_open`+`ftruncate`+`mmap` | verified on Windows |
| Vulkan loader | `dlopen` instead of `LoadLibrary`, `libvulkan.so.1`, adapter LUID optional (`PlatformAdapterLuid`) | verified |
| Vulkan extensions | Linux requests `VK_KHR_external_memory{,_fd,_dma_buf}`, `VK_EXT_external_memory_dma_buf`, `VK_EXT_image_drm_format_modifier`, `VK_KHR_external_semaphore{,_fd}` | not yet exercised |
| Shader source | HLSL moved to `include/shaders/*.hlsli`, one copy for both builds | verified |
| SPIR-V | CMake runs `dxc -spirv` and embeds the result (`cmake/EmbedSpirv.cmake`); Windows keeps runtime `D3DCompile` + `DxbcToSpv` | Linux untested |
| Eye images | `GpuImage` struct (handle + extent + format) replaces `ID3D11Texture2D*` end to end; `VideoEncoder::OpenSharedEyeTextures` deleted | verified |
| Eye import | `VulkanCompose::FindOrImportEye` has a dma-buf branch behind `#else` | Linux untested |
| Swap textures | `src/DirectModePosix.cpp`: Vulkan images + `vkGetMemoryFdKHR` export, external-semaphore sync | Linux untested |
| Encoder | `h264_vaapi` candidate via `/dev/dri/renderD128`; D3D11VA/AMF path is Windows-only | Linux untested |

Windows-only files that Linux does not compile: `DirectMode.cpp`, `DxbcToSpv.cpp`,
everything under `include/dxil-spirv/`. Everything else is in the CMake source list.

## Ordered task list for the Linux side

1. **Descriptor bindings (do this first).** `dxbc-spirv` assigns SPIR-V descriptor slots by
   first use, so the pipelines were written for graphics `0=cbuffer, 1=sampler, 2=t0,
   3=t1` and compute `0=UBO, 1=image, 2=storage`. `dxc` assigns them per register space
   instead, so the generated SPIR-V will **not** match the `VkDescriptorSetLayout` in
   `VulkanCompose.cpp`. Check what you got:
   ```bash
   spirv-dis driver_cardboardplusplus/build/spirv/nv12_cs.spv | grep -i binding
   spirv-dis driver_cardboardplusplus/build/spirv/compose_ps.spv | grep -i binding
   ```
   then align the layout on the Linux branch. `[[vk::binding(n)]]` in the `.hlsli` files
   would fix both compilers, but it was tried and **breaks the Windows path**
   (`DxbcToSpv` rejects it), so it is not in the tree.
2. **First compile.** Expect header/attribute errors; `pkg-config` names, `std::min` in
   constant expressions and missing includes are the usual ones.
3. **Swap-set handle convention.** `DirectModePosix.cpp` returns the dma-buf fd as
   `rSharedTextureHandles[i]` and `VulkanCompose::FindOrImportEye` imports it with
   `VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT`. If SteamVR's compositor on Linux
   instead expects the raw `VkImage` handle, `CreateSwapTextureSet` is the only place to
   change. Confirm against what `vrserver.sh` logs for an unknown handle.
4. **DRM format modifier.** `FindOrImportEye` uses `VkImageDrmFormatModifierExplicitCreateInfoEXT`
   with `DRM_FORMAT_MOD_INVALID`. If `vkCreateImage` fails, query the supported modifiers
   with `vkGetPhysicalDeviceImageFormatProperties2` + `VK_IMAGE_FORMAT_PROPERTIES_DMA_BUF_MODIFIER_BIT_EXT`
   and use the fd's real modifier (needs `VK_EXT_image_format_reinterpret`).
5. **Sync semaphore.** `AcquireSyncTexture` imports the compositor's handle with
   `vkImportSemaphoreFdKHR`. If the handle is not an fd semaphore, this is where the
   mismatch shows up.
6. **Encoder.** With `useGpuEncoding` off, `libx264` encodes the host-visible NV12 that the
   Vulkan compute pass produces, which needs nothing new. Turn GPU encoding on only after
   `h264_vaapi` opens.
7. **Bridge SHM name.** The driver uses `/cardboard_pp_bridge` and `/cardboard_pp_bridge_cmd`
   on Linux. The Rust bridge opens the Windows names, so `bridge/crates/...` needs the
   POSIX names (same `BridgeProtocol.h` layout, byte for byte) or the driver will publish
   nothing and the UI will show `driver_connected: false`.

## Do not change

- `driver_cardboardplusplus/include/CardboardWire.h` — locked UDP contract.
- `driver_cardboardplusplus/include/BridgeProtocol.h` layout — mirrored by `protocol.rs`.
- Anything in `cardboardplusplus-android/` — the phone side is platform-neutral.

## Reference

- Windows verification recipe: `scripts/compile-driver.ps1` → `scripts/install-driver.ps1`
  → restart SteamVR → `vrserver.txt` shows `VK-COMPOSE: frame=<n>`, bridge `/preview`
  reports 60 fps and 0 drops.
- `docs/PROJECT_VISION.md` explains the component split.