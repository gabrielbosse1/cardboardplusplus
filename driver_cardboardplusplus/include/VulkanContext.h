#pragma once
#include <cstdint>
#include "Platform.h"
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

// Adapter identity used to pin the Vulkan device to the same GPU as the graphics
// device. Windows passes the host adapter id; Linux has no host graphics stack,
// so callers pass nullptr and the first device with a graphics queue wins.
#ifdef _WIN32
using PlatformAdapterId = LUID;
#else
struct PlatformAdapterId {
    uint32_t LowPart;
    int32_t HighPart;
};
#endif

// Vulkan device for the video path; the loader is opened at runtime so the driver
// still loads when no Vulkan runtime is installed (IsReady() stays false and
// callers drop frames instead of rendering).
class VulkanContext {
public:
    // Creates instance/device/queue on the physical device matching d3dLuid when
    // one is given; a null luid selects the first device exposing a graphics queue.
    bool Initialize(const PlatformAdapterId* d3dLuid);
    void Destroy();
    bool IsReady() const { return m_instance != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE; }
    uint32_t GetQueueFamily() const { return m_queueFamily; }
// Device pointers for share code (importing external images, host copy readbacks).
    VkPhysicalDevice Physical() const { return m_physical; }
    VkDevice Device() const { return m_device; }
    VkQueue Queue() const { return m_queue; }
    // Device-level command lookup; null when Vulkan failed to initialize.
    void* GetDeviceProc(const char* name);
    // Instance-level command lookup (physical-device commands); null when Vulkan failed.
    void* GetInstanceProc(const char* name);
// Extension support recorded at init; importing external images needs the matching pair.
    bool HasExternalMemoryWin32() const { return m_hasExternalMemoryWin32; }
    bool HasKeyedMutex() const { return m_hasKeyedMutex; }
    bool HasExternalMemoryFd() const { return m_hasExternalMemoryFd; }
    bool HasExternalSemaphoreFd() const { return m_hasExternalSemaphoreFd; }
    bool HasLuidMatch() const { return m_luidMatched; }
private:
    cbpp::PlatformModule m_lib = nullptr;
    PFN_vkGetInstanceProcAddr m_vkGetInstanceProcAddr = nullptr;
    PFN_vkGetDeviceProcAddr m_vkGetDeviceProcAddr = nullptr;
    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_queueFamily = 0;
    bool m_luidMatched = false;
    bool m_hasExternalMemoryWin32 = false;
    bool m_hasKeyedMutex = false;
    bool m_hasExternalMemoryFd = false;
    bool m_hasExternalSemaphoreFd = false;
};