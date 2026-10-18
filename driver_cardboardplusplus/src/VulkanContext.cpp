#include "VulkanContext.h"
#include "DriverLog.h"
#include <cstring>
#include <vector>
// Commands fetched per-instance; everything else goes through vkGetInstanceProcAddr/vkGetDeviceProcAddr at the call sites that need it.
namespace {
// Function-pointer typedefs for commands this file calls; declared here because
// VK_NO_PROTOTYPES means the loader headers do not declare them as symbols.
typedef PFN_vkCreateInstance PFN_vkCreateInstance_;
typedef PFN_vkEnumerateInstanceVersion PFN_vkEnumerateInstanceVersion_;
typedef PFN_vkEnumeratePhysicalDevices PFN_vkEnumeratePhysicalDevices_;
typedef PFN_vkGetPhysicalDeviceProperties PFN_vkGetPhysicalDeviceProperties_;
typedef PFN_vkGetPhysicalDeviceQueueFamilyProperties PFN_vkGetPhysicalDeviceQueueFamilyProperties_;
typedef PFN_vkGetPhysicalDeviceProperties2 PFN_vkGetPhysicalDeviceProperties2_;
typedef PFN_vkEnumerateDeviceExtensionProperties PFN_vkEnumerateDeviceExtensionProperties_;
typedef PFN_vkCreateDevice PFN_vkCreateDevice_;
typedef PFN_vkGetDeviceQueue PFN_vkGetDeviceQueue_;
typedef PFN_vkDestroyInstance PFN_vkDestroyInstance_;
typedef PFN_vkDestroyDevice PFN_vkDestroyDevice_;
static PFN_vkEnumerateInstanceVersion_ s_vkEnumerateInstanceVersion;
static PFN_vkEnumeratePhysicalDevices_ s_vkEnumeratePhysicalDevices;
static PFN_vkGetPhysicalDeviceProperties_ s_vkGetPhysicalDeviceProperties;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties_ s_vkGetPhysicalDeviceQueueFamilyProperties;
static PFN_vkGetPhysicalDeviceProperties2_ s_vkGetPhysicalDeviceProperties2;
static PFN_vkEnumerateDeviceExtensionProperties_ s_vkEnumerateDeviceExtensionProperties;
static PFN_vkCreateDevice_ s_vkCreateDevice;
static PFN_vkGetDeviceQueue_ s_vkGetDeviceQueue;
static PFN_vkDestroyInstance_ s_vkDestroyInstance;
static PFN_vkDestroyDevice_ s_vkDestroyDevice;
}  // namespace
// Opens the Vulkan loader for the running OS, enumerates physical devices, and picks the one sharing the
// graphics adapter LUID when the platform provides one.
bool VulkanContext::Initialize(const PlatformAdapterId* adapterId)
{
    Destroy();
    m_luidMatched = false;
#ifdef _WIN32
    const char* loaderName = "vulkan-1.dll";
#else
    const char* loaderName = "libvulkan.so.1";
#endif
    m_lib = cbpp::PlatformLoadModule(loaderName);
    if (!m_lib) {
        DriverLog("Vulkan: %s not found; Vulkan path disabled", loaderName);
        return false;
    }
    m_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)cbpp::PlatformGetSymbol(m_lib, "vkGetInstanceProcAddr");
    if (!m_vkGetInstanceProcAddr) {
        DriverLog("Vulkan: vkGetInstanceProcAddr missing");
        Destroy();
        return false;
    }
    auto vkCreateInstance = (PFN_vkCreateInstance)m_vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    s_vkEnumerateInstanceVersion = (PFN_vkEnumerateInstanceVersion)m_vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    if (!vkCreateInstance) {
        DriverLog("Vulkan: vkCreateInstance missing");
        Destroy();
        return false;
    }
    uint32_t apiVersion = VK_API_VERSION_1_1;
    if (s_vkEnumerateInstanceVersion) {
        uint32_t v = 0;
        if (s_vkEnumerateInstanceVersion(&v) == VK_SUCCESS) apiVersion = v;
    }
    VkApplicationInfo app = {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "cardboardplusplus-driver";
    app.apiVersion = apiVersion;
    const char* instanceExts[] = { VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME };
    VkInstanceCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 1;
    ici.ppEnabledExtensionNames = instanceExts;
    VkResult res = vkCreateInstance(&ici, nullptr, &m_instance);
    if (res != VK_SUCCESS) {
        DriverLog("Vulkan: vkCreateInstance failed (%d)", res);
        Destroy();
        return false;
    }
    m_vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_vkGetInstanceProcAddr(m_instance, "vkGetDeviceProcAddr");
#define VKINST(name, pfnType, strName) name = (pfnType)m_vkGetInstanceProcAddr(m_instance, strName)
    VKINST(s_vkEnumeratePhysicalDevices, PFN_vkEnumeratePhysicalDevices, "vkEnumeratePhysicalDevices");
    VKINST(s_vkGetPhysicalDeviceProperties, PFN_vkGetPhysicalDeviceProperties, "vkGetPhysicalDeviceProperties");
    VKINST(s_vkGetPhysicalDeviceQueueFamilyProperties, PFN_vkGetPhysicalDeviceQueueFamilyProperties, "vkGetPhysicalDeviceQueueFamilyProperties");
    VKINST(s_vkGetPhysicalDeviceProperties2, PFN_vkGetPhysicalDeviceProperties2, "vkGetPhysicalDeviceProperties2");
    VKINST(s_vkEnumerateDeviceExtensionProperties, PFN_vkEnumerateDeviceExtensionProperties, "vkEnumerateDeviceExtensionProperties");
    VKINST(s_vkCreateDevice, PFN_vkCreateDevice, "vkCreateDevice");
    if (!s_vkEnumeratePhysicalDevices || !s_vkGetPhysicalDeviceProperties || !s_vkCreateDevice) {
        DriverLog("Vulkan: core instance commands missing");
        Destroy();
        return false;
    }
    uint32_t count = 0;
    res = s_vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (res != VK_SUCCESS || count == 0) {
        DriverLog("Vulkan: no physical devices");
        Destroy();
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    res = s_vkEnumeratePhysicalDevices(m_instance, &count, devices.data());
    if (res != VK_SUCCESS) {
        DriverLog("Vulkan: physical device enumeration failed (%d)", res);
        Destroy();
        return false;
    }
    // Prefer the adapter matching the graphics device LUID; remember the first
    // device as fallback when no adapter id match exists (or adapterId is null).
    for (uint32_t i = 0; i < count && !m_physical; i++) {
        VkPhysicalDeviceProperties props = {};
        s_vkGetPhysicalDeviceProperties(devices[i], &props);
        if (!adapterId || (adapterId->LowPart == 0 && adapterId->HighPart == 0)) {
            m_physical = devices[i];
            DriverLog("Vulkan: using first device %s (no adapter LUID)",
                      props.deviceName);
            break;
        }
        VkPhysicalDeviceIDProperties idProps = {};
        idProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 props2 = {};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &idProps;
        if (s_vkGetPhysicalDeviceProperties2) {
            s_vkGetPhysicalDeviceProperties2(devices[i], &props2);
            if (idProps.deviceLUIDValid == VK_TRUE &&
                std::memcmp(idProps.deviceLUID, adapterId, sizeof(PlatformAdapterId)) == 0) {
                m_physical = devices[i];
                m_luidMatched = true;
                DriverLog("Vulkan: matched adapter LUID on %s",
                          props.deviceName);
            }
        }
    }
    if (!m_physical) {
        DriverLog("Vulkan: no device matches the adapter LUID; using first device (import disabled)");
        m_physical = devices[0];
        m_luidMatched = false;
    }
    // Record extension support and build the enabled-device-extension list in
    // one pass; only request what the device exposes or vkCreateDevice fails.
    // Windows imports shared textures by KMT handle; Linux imports dma-buf
    // fds and semaphores by fd, so each platform requests its own set.
#ifdef _WIN32
    const char* wantedExts[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME,
    };
    const uint32_t kWantedExtCount = 3;
#else
    const char* wantedExts[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    };
    const uint32_t kWantedExtCount = 7;
#endif
    const char* enabledExts[8] = {};
    uint32_t n = 0;
    if (s_vkEnumerateDeviceExtensionProperties) {
        uint32_t cnt = 0;
        if (s_vkEnumerateDeviceExtensionProperties(m_physical, nullptr, &cnt, nullptr) == VK_SUCCESS && cnt > 0) {
            std::vector<VkExtensionProperties> exts(cnt);
            if (s_vkEnumerateDeviceExtensionProperties(m_physical, nullptr, &cnt, exts.data()) == VK_SUCCESS) {
                for (const auto& e : exts) {
                    for (uint32_t w = 0; w < kWantedExtCount; w++) {
                        if (std::strcmp(e.extensionName, wantedExts[w]) == 0 && n < 8)
                            enabledExts[n++] = wantedExts[w];
                    }
                    if (std::strcmp(e.extensionName, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME) == 0)
                        m_hasExternalMemoryWin32 = true;
                    else if (std::strcmp(e.extensionName, VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME) == 0)
                        m_hasKeyedMutex = true;
                    else if (std::strcmp(e.extensionName, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) == 0)
                        m_hasExternalMemoryFd = true;
                    else if (std::strcmp(e.extensionName, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME) == 0)
                        m_hasExternalSemaphoreFd = true;
                }
            }
        }
    }
    DriverLog("Vulkan: ext-memory-win32=%d keyed-mutex=%d ext-memory-fd=%d ext-semaphore-fd=%d",
              m_hasExternalMemoryWin32 ? 1 : 0, m_hasKeyedMutex ? 1 : 0,
              m_hasExternalMemoryFd ? 1 : 0, m_hasExternalSemaphoreFd ? 1 : 0);
    // One graphics+compute queue family; compute matters for the NV12 convert shader in slice 4.
    uint32_t qCount = 0;
    s_vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> queues(qCount ? qCount : 1);
    if (qCount == 0) { qCount = 1; }
    s_vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &qCount, queues.data());
    int picked = -1;
    for (uint32_t i = 0; i < qCount; i++) {
        if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            (queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            picked = (int)i;
            break;
        }
    }
    if (picked < 0) {
        for (uint32_t i = 0; i < qCount; i++) {
            if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { picked = (int)i; break; }
        }
    }
    if (picked < 0) {
        DriverLog("Vulkan: no graphics queue");
        Destroy();
        return false;
    }
    m_queueFamily = (uint32_t)picked;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = m_queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    // the SPIR-V translator emits SPIR-V 1.6 (VulkanMemoryModel, PhysicalStorageBufferAddresses,
    // DrawParameters), so the device must opt into the matching 1.2 features.
    // Uses the individual feature structs because the vendored Vulkan headers
    // predate the promoted Vulkan11/12 aggregate structs.
    VkPhysicalDeviceVulkanMemoryModelFeatures fMemModel = {};
    fMemModel.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES;
    VkPhysicalDeviceShaderDrawParametersFeatures fDrawParams = {};
    fDrawParams.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES;
    VkPhysicalDeviceBufferDeviceAddressFeatures fBda = {};
    fBda.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    bool use12 = false;
    {
        VkPhysicalDeviceFeatures2 pf2 = {};
        pf2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        pf2.pNext = &fMemModel;
        fMemModel.pNext = &fDrawParams;
        fDrawParams.pNext = &fBda;
        auto getPhysDevFeat2 =
            (PFN_vkGetPhysicalDeviceFeatures2)m_vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceFeatures2");
        if (getPhysDevFeat2) {
            getPhysDevFeat2(m_physical, &pf2);
            if (fMemModel.vulkanMemoryModel && fDrawParams.shaderDrawParameters && fBda.bufferDeviceAddress) {
                fMemModel.vulkanMemoryModel = VK_TRUE;
                // the SPIR-V translator's IR uses Device-scope memory operations.
                if (fMemModel.vulkanMemoryModelDeviceScope) fMemModel.vulkanMemoryModelDeviceScope = VK_TRUE;
                fDrawParams.shaderDrawParameters = VK_TRUE;
                fBda.bufferDeviceAddress = VK_TRUE;
                use12 = true;
            }
        }
        DriverLog("Vulkan: spirv1.2 features memmodel=%d devscope=%d drawparams=%d bda=%d enabled=%d",
                  fMemModel.vulkanMemoryModel, fMemModel.vulkanMemoryModelDeviceScope,
                  fDrawParams.shaderDrawParameters, fBda.bufferDeviceAddress, use12);
    }
    VkDeviceCreateInfo dci = {};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = use12 ? (void*)&fMemModel : nullptr;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = n;
    dci.ppEnabledExtensionNames = n ? enabledExts : nullptr;
    res = s_vkCreateDevice(m_physical, &dci, nullptr, &m_device);
    if (res != VK_SUCCESS) {
        DriverLog("Vulkan: vkCreateDevice failed (%d)", res);
        Destroy();
        return false;
    }
    PFN_vkGetDeviceQueue getQueue = (PFN_vkGetDeviceQueue)m_vkGetInstanceProcAddr(m_instance, "vkGetDeviceQueue");
    if (!getQueue) {
        DriverLog("Vulkan: vkGetDeviceQueue missing");
        Destroy();
        return false;
    }
    getQueue(m_device, m_queueFamily, 0, &m_queue);
    VkPhysicalDeviceProperties props = {};
    s_vkGetPhysicalDeviceProperties(m_physical, &props);
    DriverLog("Vulkan: device ready (%s, queue family %u)", props.deviceName, m_queueFamily);
    return true;
}
void VulkanContext::Destroy()
{
    if (m_device && s_vkDestroyDevice) {
        s_vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
    }
    if (m_instance && s_vkDestroyInstance) {
        s_vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }
    if (m_lib) {
        cbpp::PlatformFreeModule(m_lib);
        m_lib = nullptr;
    }
    m_vkGetInstanceProcAddr = nullptr;
    m_vkGetDeviceProcAddr = nullptr;
}
// Resolves a device-level Vulkan command; returns null when Vulkan or the
// device is unavailable, and callers treat null as "feature disabled".
void* VulkanContext::GetDeviceProc(const char* name)
{
    if (!m_vkGetDeviceProcAddr || m_device == VK_NULL_HANDLE) return nullptr;
    return (void*)m_vkGetDeviceProcAddr(m_device, name);
}
// Resolves an instance-level command (vkCreateX/vkGetPhysicalDeviceX family).
void* VulkanContext::GetInstanceProc(const char* name)
{
    if (!m_vkGetInstanceProcAddr || m_instance == VK_NULL_HANDLE) return nullptr;
    return (void*)m_vkGetInstanceProcAddr(m_instance, name);
}
