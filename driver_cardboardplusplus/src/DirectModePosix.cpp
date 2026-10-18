// Linux compositor path: Vulkan-native swap textures with dma-buf export and
// external-semaphore synchronization, replacing the software11 shared textures the
// Windows build uses. SteamVR allocates nothing here — CreateSwapTextureSet
// returns our own Vulkan images as dma-buf fds, and Present hands the same fds
// to the composer so VulkanCompose can import them straight into its render pass.
#include "HmdDriver.h"
#include "DriverLog.h"
#include "DebugLog.h"
#include <vulkan/vulkan.h>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <vector>
using namespace vr;

namespace {
constexpr uint32_t kSwapImages = 3;
constexpr uint64_t kSemaphoreTimeoutNs = 1000000000ull; // 1s
}  // namespace

// Allocates one color image plus exportable device memory and returns the dma-buf
// fd the compositor needs to import it. Memory comes from a DEVICE_LOCAL|HOST_VISIBLE
// heap when the driver exposes one so the encoder can read it back on the same GPU.
static bool CreateExportImage(VulkanContext* ctx, uint32_t width, uint32_t height,
                              VkFormat format, VkImage* outImage, VkDeviceMemory* outMem,
                              int* outFd)
{
    auto createImage = (PFN_vkCreateImage)ctx->GetDeviceProc("vkCreateImage");
    auto getReq = (PFN_vkGetImageMemoryRequirements)ctx->GetDeviceProc("vkGetImageMemoryRequirements");
    auto getMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)ctx->GetDeviceProc("vkGetPhysicalDeviceMemoryProperties");
    auto allocMem = (PFN_vkAllocateMemory)ctx->GetDeviceProc("vkAllocateMemory");
    auto bindImage = (PFN_vkBindImageMemory)ctx->GetDeviceProc("vkBindImageMemory");
    auto getFd = (PFN_vkGetMemoryFdKHR)ctx->GetDeviceProc("vkGetMemoryFdKHR");
    if (!createImage || !getReq || !getMemProps || !allocMem || !bindImage || !getFd) {
        DriverLog("DirectMode: memory/image commands missing (external fd export unavailable)");
        return false;
    }
    VkImageCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = { width, height, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img = VK_NULL_HANDLE;
    if (createImage(ctx->Device(), &ici, nullptr, &img) != VK_SUCCESS) {
        DriverLog("DirectMode: vkCreateImage(%ux%u %u) failed", width, height, (uint32_t)format);
        return false;
    }
    VkMemoryRequirements req = {};
    getReq(ctx->Device(), img, &req);

    VkPhysicalDeviceMemoryProperties memProps = {};
    getMemProps(ctx->Physical(), &memProps);
    const uint32_t preferred = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    const uint32_t fallback = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t chosen = UINT32_MAX;
    for (uint32_t pass = 0; pass < 2 && chosen == UINT32_MAX; pass++) {
        uint32_t want = pass == 0 ? preferred : fallback;
        for (uint32_t t = 0; t < memProps.memoryTypeCount; t++) {
            if ((req.memoryTypeBits & (1u << t)) && (memProps.memoryTypes[t].propertyFlags & want) == want) {
                chosen = t;
                break;
            }
        }
    }
    if (chosen == UINT32_MAX) {
        DriverLog("DirectMode: no exportable memory type for %ux%u", width, height);
        return false;
    }
    VkExportMemoryAllocateInfo exportInfo = {};
    exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.pNext = &exportInfo;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = chosen;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (allocMem(ctx->Device(), &ai, nullptr, &mem) != VK_SUCCESS) {
        DriverLog("DirectMode: vkAllocateMemory(exportable) failed");
        return false;
    }
    if (bindImage(ctx->Device(), img, mem, 0) != VK_SUCCESS) {
        DriverLog("DirectMode: vkBindImageMemory failed");
        return false;
    }
    VkMemoryGetFdInfoKHR fdInfo = {};
    fdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fdInfo.memory = mem;
    fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    int fd = -1;
    if (getFd(ctx->Device(), &fdInfo, &fd) != VK_SUCCESS) {
        DriverLog("DirectMode: vkGetMemoryFdKHR failed (is VK_KHR_external_memory_fd enabled?)");
        return false;
    }
    *outImage = img;
    *outMem = mem;
    *outFd = fd;
    return true;
}

// Called by the compositor for each application: allocates one triple-buffered set
// and publishes the dma-buf fds as the shared texture handles.
void HmdDriver::CreateSwapTextureSet(uint32_t unPid, const SwapTextureSetDesc_t* pDesc,
                                     SwapTextureSet_t* pOutSwapTextureSet)
{
    if (!pDesc || !pOutSwapTextureSet || !m_vulkan.IsReady()) return;
    uint32_t width = pDesc->nWidth ? pDesc->nWidth : (uint32_t)m_encoderW;
    uint32_t height = pDesc->nHeight ? pDesc->nHeight : (uint32_t)m_encoderH;
    VkFormat format = (VkFormat)pDesc->nFormat;
    if (format == (VkFormat)0) format = VK_FORMAT_B8G8R8A8_UNORM;
    if (format != VK_FORMAT_B8G8R8A8_UNORM && format != VK_FORMAT_B8G8R8A8_SRGB &&
        format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_R8G8B8A8_SRGB) {
        DriverLog("DirectMode: unsupported swap-chain format %u; using B8G8R8A8_UNORM", (uint32_t)format);
        format = VK_FORMAT_B8G8R8A8_UNORM;
    }
    auto set = std::make_shared<SwapTextureSet>();
    set->width = width;
    set->height = height;
    set->format = (uint32_t)format;
    for (uint32_t i = 0; i < kSwapImages; i++) {
        VkImage img = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        int fd = -1;
        if (!CreateExportImage(&m_vulkan, width, height, format, &img, &mem, &fd)) {
            // Roll back the images already created for this set.
            auto destroyImage = (PFN_vkDestroyImage)m_vulkan.GetDeviceProc("vkDestroyImage");
            auto freeMem = (PFN_vkFreeMemory)m_vulkan.GetDeviceProc("vkFreeMemory");
            for (uint32_t j = 0; j < i; j++) {
                if (destroyImage) destroyImage(m_vulkan.Device(), set->images[j], nullptr);
                if (freeMem) freeMem(m_vulkan.Device(), set->memory[j], nullptr);
                if (set->fds[j] >= 0) ::close(set->fds[j]);
            }
            DriverLog("DirectMode: swap set creation failed, frame dropped");
            return;
        }
        set->images[i] = img;
        set->memory[i] = mem;
        set->fds[i] = fd;
        pOutSwapTextureSet->rSharedTextureHandles[i] = (vr::SharedTextureHandle_t)(uintptr_t)fd;
    }
    pOutSwapTextureSet->sharedHandleDepth = INVALID_SHARED_TEXTURE_HANDLE;
    m_swapTextureSets[unPid].push_back(set);
    for (uint32_t i = 0; i < kSwapImages; i++)
        m_setByHandle[pOutSwapTextureSet->rSharedTextureHandles[i]] = set;
    DriverLog("DirectMode: swap set %ux%u fmt=%u for pid=%u (%u dma-bufs)", width, height,
              (uint32_t)format, unPid, kSwapImages);
}

void HmdDriver::DestroySwapTextureSet(vr::SharedTextureHandle_t sharedTextureHandle)
{
    auto it = m_setByHandle.find(sharedTextureHandle);
    if (it == m_setByHandle.end()) return;
    auto set = it->second;
    for (uint32_t i = 0; i < kSwapImages; i++) m_setByHandle.erase((vr::SharedTextureHandle_t)(uintptr_t)set->fds[i]);
    auto destroyImage = (PFN_vkDestroyImage)m_vulkan.GetDeviceProc("vkDestroyImage");
    auto freeMem = (PFN_vkFreeMemory)m_vulkan.GetDeviceProc("vkFreeMemory");
    for (uint32_t i = 0; i < kSwapImages; i++) {
        if (set->images[i] && destroyImage) destroyImage(m_vulkan.Device(), set->images[i], nullptr);
        if (set->memory[i] && freeMem) freeMem(m_vulkan.Device(), set->memory[i], nullptr);
        if (set->fds[i] >= 0) ::close(set->fds[i]);
        set->images[i] = VK_NULL_HANDLE;
        set->memory[i] = VK_NULL_HANDLE;
        set->fds[i] = -1;
    }
    m_setByHandle.erase(it);
}

void HmdDriver::DestroyAllSwapTextureSets(uint32_t unPid)
{
    auto it = m_swapTextureSets.find(unPid);
    if (it == m_swapTextureSets.end()) return;
    for (auto& set : it->second) {
        for (uint32_t i = 0; i < kSwapImages; i++) {
            if (set->fds[i] < 0) continue;
            DestroySwapTextureSet((vr::SharedTextureHandle_t)(uintptr_t)set->fds[i]);
        }
    }
    m_swapTextureSets.erase(it);
}

void HmdDriver::GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t sharedTextureHandles[2],
                                           uint32_t (*pIndices)[2])
{
    pIndices[0][0] = 0;
    pIndices[1][0] = 0;
    auto it = m_setByHandle.find(sharedTextureHandles[0]);
    if (it != m_setByHandle.end()) {
        auto set = it->second;
        set->nextIndex = (set->nextIndex + 1) % kSwapImages;
        pIndices[0][0] = set->nextIndex;
        pIndices[1][0] = (set->nextIndex + 1) % kSwapImages;
    } else {
        DriverLog("GetNextSwapTextureSetIndex: unknown handle %llu", (unsigned long long)sharedTextureHandles[0]);
    }
}

// Queues one compositor layer per eye; Present consumes the list and hands the
// dma-buf fds to the Vulkan composer without any intermediate copy.
void HmdDriver::SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2])
{
    std::lock_guard<std::mutex> lock(m_submitLayersMutex);
    m_submitLayers.clear();
    SubmitLayerInfo info;
    info.hTextureLeft = perEye[0].hTexture;
    info.hTextureRight = perEye[1].hTexture;
    info.boundsLeft = perEye[0].vrTextureBounds;
    info.boundsRight = perEye[1].vrTextureBounds;
    m_submitLayers.push_back(info);
    m_hasSubmit.store(true, std::memory_order_release);
}

// Composites the queued layers into the encoder frame. The compositor's own sync
// primitive arrives as an external semaphore fd; waiting on it here is what keeps
// the imported images from being read mid-render.
void HmdDriver::Present(vr::SharedTextureHandle_t syncTexture)
{
    m_presentCount++;
    if (!m_vulkan.IsReady() || !m_encoderInitialized || !m_pVideoEncoder) {
        m_hasSubmit.store(false, std::memory_order_release);
        return;
    }
    if (!AcquireSyncTexture(syncTexture)) {
        m_hasSubmit.store(false, std::memory_order_release);
        return;
    }
    std::vector<SubmitLayerInfo> layers;
    {
        std::lock_guard<std::mutex> lock(m_submitLayersMutex);
        if (!m_hasSubmit.load(std::memory_order_acquire)) {
            ReleaseSyncTexture();
            return;
        }
        layers = m_submitLayers;
        m_hasSubmit.store(false, std::memory_order_release);
    }
    {
        std::lock_guard<std::mutex> lock(m_encodeMutex);
        m_pendingFrame = { m_encoderPts++, true };
        m_pendingFrame.layers.clear();
        for (const auto& l : layers) {
            auto itL = m_setByHandle.find(l.hTextureLeft);
            auto itR = m_setByHandle.find(l.hTextureRight);
            if (itL == m_setByHandle.end() || itR == m_setByHandle.end()) {
                DriverLog("Present: unknown swap handle (left=%llu right=%llu)",
                          (unsigned long long)l.hTextureLeft, (unsigned long long)l.hTextureRight);
                continue;
            }
            const auto& set = itL->second;
            PendingFrame::PendingLayer pl;
            pl.left.handle = (uint64_t)(uintptr_t)l.hTextureLeft;
            pl.left.width = set->width;
            pl.left.height = set->height;
            pl.left.format = set->format;
            pl.right.handle = (uint64_t)(uintptr_t)l.hTextureRight;
            pl.right.width = set->width;
            pl.right.height = set->height;
            pl.right.format = set->format;
            pl.boundsLeft = l.boundsLeft;
            pl.boundsRight = l.boundsRight;
            m_pendingFrame.layers.push_back(pl);
        }
        m_frameQueued = true;
    }
    m_encodeCv.notify_one();
    {
        std::lock_guard<std::mutex> lock(m_encodeDoneMutex);
        m_encodeDone = false;
    }
    ReleaseSyncTexture();
}

void HmdDriver::PostPresent()
{
    // Frame timing is reported from the compositor, not from here.
}

// Imports the compositor's sync handle as an external semaphore and waits on it.
// Reuses the imported semaphore while the handle stays the same, because importing
// per frame would leak one semaphore per frame.
bool HmdDriver::AcquireSyncTexture(vr::SharedTextureHandle_t syncTexture)
{
    if (!m_vulkan.IsReady() || !syncTexture) return false;
    if (m_cachedSyncHandle != (uint64_t)syncTexture || !m_syncSemaphore) {
        if (m_syncSemaphore) {
            auto destroy = (PFN_vkDestroySemaphore)m_vulkan.GetDeviceProc("vkDestroySemaphore");
            if (destroy) destroy(m_vulkan.Device(), m_syncSemaphore, nullptr);
            m_syncSemaphore = VK_NULL_HANDLE;
        }
        auto importFd = (PFN_vkImportSemaphoreFdKHR)m_vulkan.GetDeviceProc("vkImportSemaphoreFdKHR");
        auto getFd = (PFN_vkGetSemaphoreFdKHR)m_vulkan.GetDeviceProc("vkGetSemaphoreFdKHR");
        if (!importFd) {
            DriverLog("AcquireSyncTexture: vkImportSemaphoreFdKHR missing (is VK_KHR_external_semaphore_fd enabled?)");
            return false;
        }
        // The handle is the compositor's semaphore fd; adopt it directly when the
        // spec allows, otherwise round-trip through our own import/export.
        VkSemaphoreCreateInfo sci = {};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkImportSemaphoreFdInfoKHR import = {};
        import.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
        import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT_EXT;
        import.fd = (int)syncTexture;
        sci.pNext = &import;
        VkSemaphore sem = VK_NULL_HANDLE;
        VkResult res = ((PFN_vkCreateSemaphore)m_vulkan.GetDeviceProc("vkCreateSemaphore"))(
            m_vulkan.Device(), &sci, nullptr, &sem);
        if (res != VK_SUCCESS) {
            DriverLog("AcquireSyncTexture: sync semaphore import failed (%d)", res);
            return false;
        }
        (void)getFd;
        m_syncSemaphore = sem;
        m_cachedSyncHandle = (uint64_t)syncTexture;
    }
    auto wait = (PFN_vkWaitForSemaphores)m_vulkan.GetDeviceProc("vkWaitForSemaphores");
    auto getStatus = (PFN_vkGetSemaphoreStatusKHR)m_vulkan.GetDeviceProc("vkGetSemaphoreStatusKHR");
    if (!wait) return false;
    if (getStatus && getStatus(m_vulkan.Device(), m_syncSemaphore) == VK_SIGNALED) {
        m_syncAcquired = true;
        return true;
    }
    VkSemaphoreWaitInfo wi = {};
    wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wi.semaphoreCount = 1;
    wi.pSemaphores = &m_syncSemaphore;
    wi.pValues = nullptr;
    VkResult res = wait(m_vulkan.Device(), &wi, kSemaphoreTimeoutNs);
    if (res != VK_SUCCESS) {
        static unsigned s_timeouts = 0;
        if (++s_timeouts <= 3 || s_timeouts % 300 == 0)
            DriverLog("AcquireSyncTexture: semaphore wait returned %d (drop=%u)", res, s_timeouts);
        return false;
    }
    m_syncAcquired = true;
    return true;
}

// The semaphore is consumed by the wait in AcquireSyncTexture; the compositor owns
// the fence, so there is nothing to signal back here.
void HmdDriver::ReleaseSyncTexture()
{
    m_syncAcquired = false;
}

// Blocks until the encode thread drained the previous frame, keeping the swap chain
// from recycling a texture the encoder is still importing.
void HmdDriver::WaitEncoderIdle()
{
    std::unique_lock<std::mutex> lock(m_encodeDoneMutex);
    m_encodeDoneCv.wait(lock, [this] { return m_encodeDone; });
}

void HmdDriver::GetFrameTiming(DriverDirectMode_FrameTiming* pFrameTiming)
{
    if (!pFrameTiming) return;
    pFrameTiming->flFrameTime = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    pFrameTiming->flHostTime = pFrameTiming->flFrameTime;
    pFrameTiming->flHostTimeTicks = pFrameTiming->flFrameTime;
    pFrameTiming->flCompositorTime = pFrameTiming->flFrameTime;
}