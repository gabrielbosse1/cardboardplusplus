#pragma once
#include <cstdint>

// One eye image handed to the Vulkan composer. Windows carries a legacy DXGI
// shared handle and DXGI_FORMAT for a D3D11 copy. Linux carries an opaque Vulkan
// memory fd and VkFormat from the SteamVR IPC image.
struct GpuImage {
    uint64_t handle = 0;   // Windows: KMT shared handle. Linux: opaque memory fd.
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;   // Windows: DXGI_FORMAT. Linux: VkFormat.
    // True when the platform exposes a cross-API sync primitive for this image
    // (Windows sync object, Linux external semaphore) that must be held while sampling.
    bool synchronized = false;
};