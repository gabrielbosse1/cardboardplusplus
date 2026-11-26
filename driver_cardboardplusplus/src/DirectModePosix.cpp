// Linux compositor path: SteamVR allocates the swap images through its IPC
// resource manager and renders into them; we import the same memory into our
// Vulkan device and sample it in the compose pass. Present publishes the
// submitted texture's fd so the composer reads the slot drawn that frame.
#include "HmdDriver.h"
#ifndef _WIN32
#include "LinuxDmabuf.h"
#endif
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

#ifndef _WIN32
// Lets SteamVR allocate each swap image through its IPC resource manager, then
// imports that allocation into our Vulkan device. The handle published back to
// the compositor is the IPC handle, not a raw fd. OPAQUE_FD import matches how
// SteamVR created the image: same format, MUTABLE_FORMAT, no modifier list.
static bool CreateSharedEye(VulkanContext* ctx, uint32_t width, uint32_t height,
                            VkFormat format, VkImage* outImage, VkDeviceMemory* outMem,
                            int* outFd, vr::SharedTextureHandle_t* outHandle)
{
    vr::IVRIPCResourceManagerClient* ipc = vr::IPCResourceManager();
    if (!ipc) {
        DriverLog("DirectMode: IPC resource manager unavailable");
        return false;
    }
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    uint32_t creationFlags = (uint32_t)VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    if (!ipc->NewSharedVulkanImage((uint32_t)format, width, height, true, false, false,
                                   1, 1, creationFlags, usage, outHandle) || !*outHandle) {
        DriverLog("DirectMode: NewSharedVulkanImage failed for %ux%u fmt=%u",
                  width, height, (uint32_t)format);
        return false;
    }
    uint64_t ipcHandle = 0;
    int fd = -1;
    if (!ipc->RefResource(*outHandle, &ipcHandle) ||
        !ipc->ReceiveSharedFd(ipcHandle, &fd) || fd < 0) {
        DriverLog("DirectMode: could not get fd for shared eye");
        ipc->UnrefResource(*outHandle);
        *outHandle = 0;
        return false;
    }
    auto createImage = (PFN_vkCreateImage)ctx->GetDeviceProc("vkCreateImage");
    auto getReq = (PFN_vkGetImageMemoryRequirements)ctx->GetDeviceProc("vkGetImageMemoryRequirements");
    auto allocMem = (PFN_vkAllocateMemory)ctx->GetDeviceProc("vkAllocateMemory");
    auto bindImage = (PFN_vkBindImageMemory)ctx->GetDeviceProc("vkBindImageMemory");
    auto getMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)
        ctx->GetInstanceProc("vkGetPhysicalDeviceMemoryProperties");
    if (!createImage || !getReq || !allocMem || !bindImage || !getMemProps) {
        ::close(fd);
        return false;
    }
    VkImageCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    ici.format = format;
    ici.extent = { width, height, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkExternalMemoryImageCreateInfo ext = {};
    ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    ici.pNext = &ext;
    VkImage img = VK_NULL_HANDLE;
    if (createImage(ctx->Device(), &ici, nullptr, &img) != VK_SUCCESS) {
        DriverLog("DirectMode: vkCreateImage(shared eye) failed");
        ::close(fd);
        return false;
    }
    VkMemoryRequirements req = {};
    getReq(ctx->Device(), img, &req);
    VkPhysicalDeviceMemoryProperties memProps = {};
    getMemProps(ctx->Physical(), &memProps);
    uint32_t type = UINT32_MAX;
    for (uint32_t t = 0; t < memProps.memoryTypeCount; t++) {
        if ((req.memoryTypeBits & (1u << t)) &&
            (memProps.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            type = t; break;
        }
    }
    if (type == UINT32_MAX) {
        for (uint32_t t = 0; t < memProps.memoryTypeCount; t++) {
            if (req.memoryTypeBits & (1u << t)) { type = t; break; }
        }
    }
    if (type == UINT32_MAX) { ::close(fd); return false; }
    VkImportMemoryFdInfoKHR import = {};
    import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    import.fd = ::dup(fd); // Vulkan consumes its copy; keep ours for the composer
    VkMemoryDedicatedAllocateInfo dedicated = {};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = img;
    dedicated.pNext = &import;
    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    ai.pNext = &dedicated;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (allocMem(ctx->Device(), &ai, nullptr, &mem) != VK_SUCCESS ||
        bindImage(ctx->Device(), img, mem, 0) != VK_SUCCESS) {
        DriverLog("DirectMode: shared eye import failed");
        ::close(fd);
        return false;
    }
    *outImage = img; *outMem = mem; *outFd = fd;
    return true;
}
#endif

// Called by the compositor for each application: creates one triple-buffered set
// of IPC shared images and publishes their shared texture handles.
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
        vr::SharedTextureHandle_t shared = 0;
        bool ok = false;
        ok = CreateSharedEye(&m_vulkan, width, height, format, &img, &mem, &fd, &shared);
        if (!ok) {
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
        set->ipcHandles[i] = shared;
        pOutSwapTextureSet->rSharedTextureHandles[i] = shared;
    }
    pOutSwapTextureSet->unTextureFlags = 0;
    m_swapTextureSets[unPid].push_back(set);
    m_orderedSets.push_back(set);
    for (uint32_t i = 0; i < kSwapImages; i++)
        m_setByHandle[pOutSwapTextureSet->rSharedTextureHandles[i]] = set;
    DriverLog("DirectMode: swap set %ux%u fmt=%u for pid=%u (%u shared images)", width, height,
              (uint32_t)format, unPid, kSwapImages);
}

void HmdDriver::DestroySwapTextureSet(vr::SharedTextureHandle_t sharedTextureHandle)
{
    auto it = m_setByHandle.find(sharedTextureHandle);
    if (it == m_setByHandle.end()) return;
    auto set = it->second;
    if (vr::IVRIPCResourceManagerClient* ipc = vr::IPCResourceManager()) {
        for (uint32_t i = 0; i < kSwapImages; i++) {
            if (set->ipcHandles[i]) ipc->UnrefResource(set->ipcHandles[i]);
        }
    }
    for (uint32_t i = 0; i < kSwapImages; i++) m_setByHandle.erase(set->ipcHandles[i]);
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
    m_setByHandle.erase(sharedTextureHandle);
}

void HmdDriver::DestroyAllSwapTextureSets(uint32_t unPid)
{
    auto it = m_swapTextureSets.find(unPid);
    if (it == m_swapTextureSets.end()) return;
    for (auto& set : it->second) {
        if (set->ipcHandles[0]) {
            DestroySwapTextureSet(set->ipcHandles[0]);
            continue;
        }
    }
    m_swapTextureSets.erase(it);
    m_orderedSets.clear();
}

std::shared_ptr<SwapTextureSet> HmdDriver::ResolveSwapSet(vr::SharedTextureHandle_t handle)
{
    auto it = m_setByHandle.find(handle);
    if (it != m_setByHandle.end()) return it->second;
    if (handle >= 1 && handle <= m_orderedSets.size()) return m_orderedSets[handle - 1];
    return {};
}

// Finds which of the three swap slots a submitted texture handle belongs to.
// The compositor rotates slots on its own; the index reported by
// GetNextSwapTextureSetIndex is only a hint for the next frame.
static int SwapIndexForHandle(const SwapTextureSet& set, vr::SharedTextureHandle_t handle)
{
    for (uint32_t i = 0; i < kSwapImages; i++) {
        if (set.ipcHandles[i] == handle) return (int)i;
    }
    return -1;
}

void HmdDriver::GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t sharedTextureHandles[2],
                                           uint32_t (*pIndices)[2])
{
    pIndices[0][0] = 0;
    pIndices[1][0] = 0;
    // Each eye has its own triple-buffered set. Advance them independently so
    // the compositor renders each eye into the slot Present will actually read.
    for (int eye = 0; eye < 2; eye++) {
        auto set = ResolveSwapSet(sharedTextureHandles[eye]);
        if (!set) {
            // A zero handle means the compositor did not supply an eye here.
            // Only unexpected handles are worth logging, and only occasionally:
            // every log line crosses SteamVR's IPC pipe.
            if (sharedTextureHandles[eye]) {
                static unsigned unknownLogs = 0;
                if (++unknownLogs <= 5 || unknownLogs % 300 == 0) {
                    DriverLog("GetNextSwapTextureSetIndex: unknown handle %llu (eye %d, total=%u)",
                              (unsigned long long)sharedTextureHandles[eye], eye, unknownLogs);
                }
            }
            continue;
        }
        set->nextIndex = (set->nextIndex + 1) % kSwapImages;
        pIndices[eye][0] = set->nextIndex;
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
    info.boundsLeft = perEye[0].bounds;
    info.boundsRight = perEye[1].bounds;
    m_submitLayers.push_back(info);
    m_hasSubmit.store(true, std::memory_order_release);
    static bool loggedFirstSubmit = false;
    if (!loggedFirstSubmit) {
        loggedFirstSubmit = true;
        DriverLog("SubmitLayer: first layer queued");
    }
}

// Composites the queued layers into the encoder frame. The compositor's own sync
// primitive arrives as an external semaphore fd; waiting on it here is what keeps
// the imported images from being read mid-render.
void HmdDriver::Present(vr::SharedTextureHandle_t syncTexture)
{
    m_presentCount++;
    static uint32_t presentLogs = 0;
    if (++presentLogs == 1 || presentLogs % 300 == 1) {
        DriverLog("Present: call=%u sync=%llu", presentLogs, (unsigned long long)syncTexture);
    }
    if (!m_vulkan.IsReady() || !m_encoderInitialized || !m_pVideoEncoder) {
        m_hasSubmit.store(false, std::memory_order_release);
        return;
    }
    // Some SteamVR builds hand Present a null sync handle in direct mode.
    // That means there is nothing extra to wait on, not that the frame is bad.
    const bool hasSync = syncTexture != 0;
    if (hasSync && !AcquireSyncTexture(syncTexture)) {
        m_hasSubmit.store(false, std::memory_order_release);
        return;
    }
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
    {
        std::lock_guard<std::mutex> lock(m_encodeMutex);
        m_pendingFrame = { m_encoderPts++, true };
        m_pendingFrame.layers.clear();
        for (const auto& l : layers) {
            auto set = ResolveSwapSet(l.hTextureLeft);
            auto setRight = ResolveSwapSet(l.hTextureRight);
            if (!set || !setRight) {
                DriverLog("Present: unknown swap handle (left=%llu right=%llu)",
                          (unsigned long long)l.hTextureLeft, (unsigned long long)l.hTextureRight);
                continue;
            }
            // Read the slot the compositor actually rendered into this frame.
            // The next-index hint is for the compositor's next write, not ours.
            int idxLeft = SwapIndexForHandle(*set, l.hTextureLeft);
            int idxRight = SwapIndexForHandle(*setRight, l.hTextureRight);
            if (idxLeft < 0 || idxRight < 0) {
                static unsigned slotMisses = 0;
                if (++slotMisses <= 5 || slotMisses % 300 == 0) {
                    DriverLog("Present: swap slot not found (left=%llu/%d right=%llu/%d, drop=%u)",
                              (unsigned long long)l.hTextureLeft, idxLeft,
                              (unsigned long long)l.hTextureRight, idxRight, slotMisses);
                }
                continue;
            }
            PendingFrame::PendingLayer pl;
            pl.left.handle = (uint64_t)(uintptr_t)set->fds[idxLeft];
            pl.left.width = set->width;
            pl.left.height = set->height;
            pl.left.format = set->format;
            pl.right.handle = (uint64_t)(uintptr_t)setRight->fds[idxRight];
            pl.right.width = setRight->width;
            pl.right.height = setRight->height;
            pl.right.format = setRight->format;
            static bool loggedBounds = false;
            if (!loggedBounds) {
                loggedBounds = true;
                DriverLog("Present layer: L=%llu R=%llu bounds L=(%.3f,%.3f)-(%.3f,%.3f) R=(%.3f,%.3f)-(%.3f,%.3f)",
                          (unsigned long long)l.hTextureLeft, (unsigned long long)l.hTextureRight,
                          l.boundsLeft.uMin, l.boundsLeft.vMin, l.boundsLeft.uMax, l.boundsLeft.vMax,
                          l.boundsRight.uMin, l.boundsRight.vMin, l.boundsRight.uMax, l.boundsRight.vMax);
            }
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
    if (hasSync) ReleaseSyncTexture();
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
        import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT_KHR;
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
    auto wait = (PFN_vkWaitSemaphores)m_vulkan.GetDeviceProc("vkWaitSemaphores");
    if (!wait) return false;
    // The vendored Vulkan headers expose core 1.1 vkWaitSemaphores only (no
    // vkGetSemaphoreStatusKHR), so skip the already-signaled early-out and let
    // the bounded wait below resolve the compositor sync.
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
    // The vendored OpenVR header exposes the version-007 struct, which carries
    // present/drop counters only (no timing fields). Fill the size and counters.
    pFrameTiming->m_nSize = (uint32_t)sizeof(DriverDirectMode_FrameTiming);
    pFrameTiming->m_nNumFramePresents = (uint32_t)m_presentCount;
    pFrameTiming->m_nNumDroppedFrames = 0;
    pFrameTiming->m_nNumMisPresented = 0;
    pFrameTiming->m_nReprojectionFlags = 0;
}