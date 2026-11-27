#pragma once
#include <cstdint>

// Platform-neutral description of one compositor eye image handed to the Vulkan
// composer. The handle is whatever the compositor handed back for that texture:
// a host shared handle on Windows, a dma-buf file descriptor on Linux.
struct GpuImage {
    uint64_t handle = 0;   // Windows: shared host handle. Linux: dma-buf fd.
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;   // Windows: host format enum. Linux: VkFormat.
    // True when the platform exposes a cross-API sync primitive for this image
    // (Windows sync object, Linux external semaphore) that must be held while sampling.
    bool synchronized = false;
};