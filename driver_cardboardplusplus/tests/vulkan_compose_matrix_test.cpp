// Vulkan compose matrix: parameterized extension of the vulkan_pipeline_smoke
// phase B. Links the REAL production VulkanContext + VulkanCompose on lavapipe
// and pins, per resolution and layer configuration, the compose + NV12
// contracts the encoder and the phone stream depend on:
//   C16 resolution matrix vs the BT.601 oracle
//   C17 multi-layer compose (alpha overlay, 4 accepted, 5 rejected WITHOUT
//       disabling the composer — a bad layer count used to Fail() the whole
//       pipeline, a black-screen candidate at app transitions)
//   C18 per-eye degenerate policy (one broken eye must not smear its half)
//   C19 subrect bounds (0.25..0.75 windows, SBS seam, clamped edges)
//   C20 eye cache (fd-import churn bounded, prune frees stale imports)
//   C21 GPU NV12 vs CPU swscale content on the same compose
//   C37 determinism (two composes -> bit-identical NV12)
//   C38 100-frame soak (fd-stable, RSS-bounded, zero failed composes)
//   C22 KHRONOS validation layer when installed (multi-hardware proxy)
//
// Build (CI): g++ -std=c++17 -O1 -Wall -Wextra \
//   -I driver_cardboardplusplus/include -I driver_cardboardplusplus/build/spirv \
//   -o vk_matrix tests/vulkan_compose_matrix_test.cpp \
//   src/VulkanContext.cpp src/VulkanCompose.cpp \
//   -lvulkan
// Run with lavapipe: VK_ICD_FILENAMES=lvp_icd.json ./vk_matrix
#include <vulkan/vulkan.h>  // FIRST: keeps the vk* prototypes (see smoke test)

#include "VulkanContext.h"
#include "VulkanCompose.h"
#include "openvr_driver.h"

#include <cstdarg>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <dirent.h>
#include <unistd.h>

// swscale for the C21 CPU-fallback comparison (same parameters the encoder's
// fallback path uses).
extern "C" {
#include <libswscale/swscale.h>
}

// --- harness -----------------------------------------------------------------
static int g_failures = 0;
static void Check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "== PASS" : "== FAIL", what);
    if (!ok) g_failures++;
}

// Outside SteamVR the OpenVR inline accessors need a driver context; the stub
// resolves every interface to null so the code paths log to stderr.
namespace {
struct StubDriverContext final : vr::IVRDriverContext {
    void* GetGenericInterface(const char*, vr::EVRInitError*) override { return nullptr; }
    vr::DriverHandle_t GetDriverHandle() override { return 0; }
};
StubDriverContext g_stubContext;
}  // namespace
void DriverLog(const char* pFormat, ...) {
    va_list ap;
    va_start(ap, pFormat);
    vfprintf(stderr, pFormat, ap);
    va_end(ap);
    fputc('\n', stderr);
}

// --- C22: validation-error capture (multi-hardware proxy) ---------------------
namespace {
struct ValidationState {
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    std::atomic_int errors{0};
    std::atomic_int warnings{0};
};
ValidationState g_validation;

VKAPI_ATTR VkBool32 VKAPI_CALL ValidationCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        g_validation.errors.fetch_add(1);
        fprintf(stderr, "[VVL-ERROR] %s\n", data->pMessage ? data->pMessage : "");
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        g_validation.warnings.fetch_add(1);
        fprintf(stderr, "[VVL-WARN] %s\n", data->pMessage ? data->pMessage : "");
    }
    return VK_FALSE;
}
}  // namespace

static bool HasKhronosValidationLayer() {
    uint32_t n = 0;
    if (vkEnumerateInstanceLayerProperties(&n, nullptr) != VK_SUCCESS || n == 0) return false;
    std::vector<VkLayerProperties> layers(n);
    if (vkEnumerateInstanceLayerProperties(&n, layers.data()) != VK_SUCCESS) return false;
    for (const auto& l : layers) {
        if (strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) return true;
    }
    return false;
}

static void AttachValidationMessenger(VulkanContext& ctx) {
    auto create = (PFN_vkCreateDebugUtilsMessengerEXT)
        ctx.GetInstanceProc("vkCreateDebugUtilsMessengerEXT");
    if (!create || g_validation.instance != VK_NULL_HANDLE) return;
    VkDebugUtilsMessengerCreateInfoEXT ci = {};
    ci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci.pfnUserCallback = ValidationCallback;
    if (create(ctx.Instance(), &ci, nullptr, &g_validation.messenger) == VK_SUCCESS) {
        g_validation.instance = ctx.Instance();
    }
}

static void DetachValidationMessenger(VulkanContext& ctx) {
    if (g_validation.messenger == VK_NULL_HANDLE) return;
    auto destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)
        ctx.GetInstanceProc("vkDestroyDebugUtilsMessengerEXT");
    if (destroy) destroy(ctx.Instance(), g_validation.messenger, nullptr);
    g_validation.messenger = VK_NULL_HANDLE;
    g_validation.instance = VK_NULL_HANDLE;
}

// --- eye-image factory --------------------------------------------------------
struct EyeRes {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    int fd = -1;
};

static uint32_t FindMemType(VkPhysicalDevice phys, uint32_t required, uint32_t propsMask) {
    VkPhysicalDeviceMemoryProperties mp = {};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((required & (1u << i)) && (mp.memoryTypes[i].propertyFlags & propsMask) == propsMask) return i;
    }
    return 0xFFFFFFFFu;
}

// Creates an exportable eye (SteamVR IPC shape: BGRA8, dedicated allocation,
// opaque-fd export) filled with a pattern: solid(r,g,b) or a horizontal
// gradient on the red channel (for subrect-boundary assertions).
static EyeRes MakeEye(VkDevice dev, VkPhysicalDevice phys, VkQueue queue, uint32_t qfam,
                      uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b, bool gradient) {
    EyeRes e;
    auto getMemFd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR");
    if (!getMemFd) return e;
    VkExternalMemoryImageCreateInfo ext = {};
    ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkImageCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &ext;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_B8G8R8A8_UNORM;
    ici.extent = { w, h, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ici.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    if (vkCreateImage(dev, &ici, nullptr, &e.img) != VK_SUCCESS) { e.img = VK_NULL_HANDLE; return e; }
    VkMemoryRequirements req = {};
    vkGetImageMemoryRequirements(dev, e.img, &req);
    VkExportMemoryAllocateInfo exp = {};
    exp.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkMemoryDedicatedAllocateInfo ded = {};
    ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    ded.image = e.img;
    ded.pNext = &exp;
    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.pNext = &ded;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = FindMemType(phys, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (ai.memoryTypeIndex == 0xFFFFFFFFu)
        ai.memoryTypeIndex = FindMemType(phys, req.memoryTypeBits, 0);
    if (vkAllocateMemory(dev, &ai, nullptr, &e.mem) != VK_SUCCESS) { e.img = VK_NULL_HANDLE; return e; }
    if (vkBindImageMemory(dev, e.img, e.mem, 0) != VK_SUCCESS) { e.img = VK_NULL_HANDLE; return e; }

    // Staging buffer with the pattern.
    VkBufferCreateInfo bci = {};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = (VkDeviceSize)w * h * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer stage = VK_NULL_HANDLE;
    vkCreateBuffer(dev, &bci, nullptr, &stage);
    VkMemoryRequirements sreq = {};
    vkGetBufferMemoryRequirements(dev, stage, &sreq);
    VkMemoryAllocateInfo sai = {};
    sai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    sai.allocationSize = sreq.size;
    sai.memoryTypeIndex = FindMemType(phys, sreq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory smem = VK_NULL_HANDLE;
    void* smap = nullptr;
    bool staged = vkAllocateMemory(dev, &sai, nullptr, &smem) == VK_SUCCESS &&
                  vkBindBufferMemory(dev, stage, smem, 0) == VK_SUCCESS &&
                  vkMapMemory(dev, smem, 0, bci.size, 0, &smap) == VK_SUCCESS;
    if (staged) {
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                uint8_t* px = (uint8_t*)smap + ((size_t)y * w + x) * 4;
                uint8_t rr = gradient ? (uint8_t)(x * 255 / (w > 1 ? w - 1 : 1)) : r;
                px[0] = b; px[1] = g; px[2] = rr; px[3] = 255;  // BGRA
            }
        }
        VkCommandPoolCreateInfo pci = {};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.queueFamilyIndex = qfam;
        VkCommandPool pool = VK_NULL_HANDLE;
        vkCreateCommandPool(dev, &pci, nullptr, &pool);
        VkCommandBufferAllocateInfo cai = {};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(dev, &cai, &cmd);
        VkCommandBufferBeginInfo bi = {};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &bi);
        VkImageMemoryBarrier imb = {};
        imb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        imb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        imb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        imb.image = e.img;
        imb.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &imb);
        VkBufferImageCopy region = {};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { w, h, 1 };
        vkCmdCopyBufferToImage(cmd, stage, e.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);
        vkDestroyCommandPool(dev, pool, nullptr);
    }
    vkDestroyBuffer(dev, stage, nullptr);
    vkFreeMemory(dev, smem, nullptr);
    VkMemoryGetFdInfoKHR fdInfo = {};
    fdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fdInfo.memory = e.mem;
    fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    if (getMemFd(dev, &fdInfo, &e.fd) != VK_SUCCESS) e.fd = -1;
    return e;
}

// --- long-running fixtures ----------------------------------------------------
static VulkanContext g_ctx;
static uint32_t g_qfam = 0;
static VkQueue g_queue = VK_NULL_HANDLE;
static VkDevice g_dev = VK_NULL_HANDLE;
static VkPhysicalDevice g_phys = VK_NULL_HANDLE;

struct ComposeCase {
    VulkanCompose compose;
    bool ready = false;
};

static ComposeCase MakeComposer(uint32_t w, uint32_t h) {
    ComposeCase c;
    c.compose.Init(&g_ctx);
    c.ready = c.compose.Setup(w, h) && c.compose.IsReady();
    return c;
}

static long FdCount() {
    long n = 0;
    if (DIR* d = opendir("/proc/self/fd")) {
        while (readdir(d)) n++;
        closedir(d);
    }
    return n;
}

static size_t RssKb() {
    FILE* f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    long total = 0, rss = 0;
    if (fscanf(f, "%ld %ld", &total, &rss) != 2) rss = 0;
    fclose(f);
    return (size_t)rss;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("== Vulkan compose matrix (production VulkanCompose on lavapipe) ==\n");
    vr::VRDriverContext() = &g_stubContext;

    Check(g_ctx.Initialize(nullptr), "VulkanContext::Initialize(nullptr) on lavapipe");
    if (!g_ctx.IsReady()) return 1;
    g_dev = g_ctx.Device();
    g_phys = g_ctx.Physical();
    g_queue = g_ctx.Queue();
    g_qfam = g_ctx.GetQueueFamily();

    // C22: attach a validation messenger when the layer is installed (CI can
    // install vulkan-validationlayers; without it this is a documented skip).
    bool vvl = HasKhronosValidationLayer();
    printf("== KHRONOS validation layer: %s\n", vvl ? "present" : "not installed (skipped)");
    if (vvl) AttachValidationMessenger(g_ctx);

    const uint32_t EW = 128, EH = 128;
    EyeRes red = MakeEye(g_dev, g_phys, g_queue, g_qfam, EW, EH, 255, 0, 0, false);
    EyeRes green = MakeEye(g_dev, g_phys, g_queue, g_qfam, EW, EH, 0, 255, 0, false);
    EyeRes grad = MakeEye(g_dev, g_phys, g_queue, g_qfam, EW, EH, 0, 0, 0, true);
    Check(red.fd >= 0 && green.fd >= 0 && grad.fd >= 0, "exportable eyes created");
    if (red.fd < 0 || green.fd < 0 || grad.fd < 0) return 1;
    GpuImage gRed = {};  gRed.handle = (uint64_t)red.fd;  gRed.width = EW; gRed.height = EH;
    gRed.format = (uint32_t)VK_FORMAT_B8G8R8A8_UNORM;
    GpuImage gGreen = gRed; gGreen.handle = (uint64_t)green.fd;
    GpuImage gGrad = gRed;  gGrad.handle = (uint64_t)grad.fd;

    auto fullRects = []() {
        return std::pair<std::vector<LayerRect>, std::vector<LayerRect>>{
            { LayerRect{ 0, 0, 1, 1 } }, { LayerRect{ 0, 0, 1, 1 } } };
    };

    // ---- C16: resolution matrix ----
    struct ResCase { uint32_t w, h; bool gpuNv12; const char* what; };
    // GPU NV12 needs w%4==0 and h%2==0; compose itself works for any dims
    // (odd cases stay on the CPU swscale path in the encoder).
    ResCase rc[] = {
        { 1920, 1080, true,  "C16 1920x1080 (production default, floors to 1072 in the encoder)" },
        {  512,  512, true,  "C16 512x512" },
        {  256,  256, true,  "C16 256x256" },
        { 1024,  768, true,  "C16 1024x768" },
        {  516,  258, true,  "C16 516x258 (w%4==0 edge)" },
        { 1024,  767, false, "C16 1024x767 (h odd -> GPU NV12 refused, compose ready)" },
        {  514,  256, false, "C16 514x256 (w%4==2 -> GPU NV12 refused)" },
    };
    for (const ResCase& c : rc) {
        ComposeCase cc = MakeComposer(c.w, c.h);
        Check(cc.ready, c.what);
        if (!cc.ready) continue;
        auto [lb, rb] = fullRects();
        bool composed = cc.compose.Compose({ gRed }, { gGreen }, lb, rb);
        Check(composed, "C16 compose ok");
        const uint8_t* nv12 = nullptr;
        size_t nv12Size = 0;
        bool gotNv12 = composed && cc.compose.ToNv12(&nv12, &nv12Size);
        if (c.gpuNv12) {
            Check(gotNv12 && nv12 != nullptr, "C16 GPU NV12 produced");
        } else {
            Check(!gotNv12, "C16 GPU NV12 refused by the dims guard");
        }
        if (gotNv12 && nv12) {
            // BT.601 oracle: red left half ~81.5 luma, green right half ~144.6.
            float yL = (float)nv12[(size_t)8 * c.w + c.w / 16];
            float yR = (float)nv12[(size_t)8 * c.w + c.w - c.w / 16];
            Check(yL > 77.5f && yL < 85.5f, "C16 Y left red ~81.5");
            Check(yR > 140.6f && yR < 148.6f, "C16 Y right green ~144.6");
            // No poison: every sampled Y byte inside the pl BT.601 range.
            bool inRange = true;
            size_t badIdx = 0; int badVal = 0;
            for (size_t i = 0; i < (size_t)c.w * c.h; i += 997) {
                if (nv12[i] < 8 || nv12[i] > 250) { inRange = false; badIdx = i; badVal = nv12[i]; break; }
            }
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "C16 NV12 samples in the legal Y range (%ux%u firstBad=%zu val=%d)",
                     c.w, c.h, badIdx, badVal);
            Check(inRange, msg);
        } else {
            cc.compose.CopyToHost();
            const uint8_t* host = cc.compose.HostData();
            Check(host != nullptr, "C16 host copy available for the CPU path");
            if (host) {
                const uint8_t* bl = host + ((size_t)(c.h - 1) * c.w + 8) * 4;
                const uint8_t* br = host + ((size_t)(c.h - 1) * c.w + c.w - 8) * 4;
                Check(bl[2] > 200 && bl[1] < 60 && br[1] > 200 && br[2] < 60,
                      "C16 host corners: left red / right green");
            }
        }
        cc.compose.Shutdown();
    }

    // ---- C17: multi-layer compose (alpha overlay, layer-count policy) ----
    {
        ComposeCase cc = MakeComposer(512, 256);
        Check(cc.ready, "C17 composer setup");
        if (cc.ready) {
            // Bounds are PER LAYER: base full-frame, overlay with a subrect
            // window (center half of each eye). (Passing only the base rects
            // here made layer 1 fall back to a default full rect and covered
            // the whole frame - found by this suite's first CI run.)
            std::vector<LayerRect> lb{ LayerRect{ 0, 0, 1, 1 }, LayerRect{ 0.25f, 0.25f, 0.75f, 0.75f } };
            std::vector<LayerRect> rb{ LayerRect{ 0, 0, 1, 1 }, LayerRect{ 0.25f, 0.25f, 0.75f, 0.75f } };
            bool two = cc.compose.Compose({ gRed, gGrad }, { gGreen, gGreen }, lb, rb);
            Check(two, "C17 2-layer compose (base opaque + overlay) accepted");
            const uint8_t* nv12 = nullptr;
            size_t sz = 0;
            bool got = two && cc.compose.ToNv12(&nv12, &sz);
            Check(got, "C17 NV12 after overlay compose");
            if (got && nv12) {
                // Overlay draws the gradient (blue eye w/ red gradient channel)
                // with src-alpha 255 over the center window of each half.
                // Outside the window the base layer stays.
                float baseL = (float)nv12[(size_t)16 * 512 + 16];
                // Overlay window on the left half: x in [64..191]; at the
                // center x=128 the gradient eye is sampled at u=0.5 (red=128).
                // Luma path: linear 128/255 -> sRGB ~0.74 (188) -> BT.601
                // limited Y ~16 + 65.5*0.737 = 64.3.
                float overL = (float)nv12[(size_t)128 * 512 + 128];
                Check(baseL > 77.0f && baseL < 86.0f, "C17 outside the overlay window the base shows (red)");
                char msg[160];
                snprintf(msg, sizeof(msg),
                         "C17 overlay region shows the gradient (center window replaced the base, overL=%.1f)",
                         overL);
                Check(overL > 56.0f && overL < 72.0f, msg);
            }
            // kMaxLayers = 4 accepted.
            bool four = cc.compose.Compose(
                { gRed, gRed, gRed, gRed }, { gGreen, gGreen, gGreen, gGreen },
                { LayerRect{0,0,1,1}, LayerRect{0,0,1,1}, LayerRect{0,0,1,1}, LayerRect{0,0,1,1} },
                { LayerRect{0,0,1,1}, LayerRect{0,0,1,1}, LayerRect{0,0,1,1}, LayerRect{0,0,1,1} });
            Check(four, "C17 4 layers (kMaxLayers) accepted");
            // 5 layers: rejected WITHOUT disabling the composer (the old
            // Fail() path killed the whole pipeline for the rest of the
            // session — a black-screen candidate when an app over-submits).
            bool five = cc.compose.Compose(
                { gRed, gRed, gRed, gRed, gRed },
                { gGreen, gGreen, gGreen, gGreen, gGreen },
                { LayerRect{0,0,1,1}, LayerRect{0,0,1,1}, LayerRect{0,0,1,1}, LayerRect{0,0,1,1}, LayerRect{0,0,1,1} },
                { LayerRect{0,0,1,1}, LayerRect{0,0,1,1}, LayerRect{0,0,1,1}, LayerRect{0,0,1,1}, LayerRect{0,0,1,1} });
            Check(!five, "C17 5 layers rejected");
            Check(cc.compose.IsReady(), "C17 composer STAYS ready after the rejection (no Fail() on count mismatch)");
            // Mismatched L/R counts: same policy.
            bool mismatch = cc.compose.Compose({ gRed }, { gGreen, gGreen }, lb, rb);
            Check(!mismatch && cc.compose.IsReady(), "C17 mismatched layer counts rejected, composer stays ready");
            // And the composer still composes after both rejections.
            auto [lb2, rb2] = fullRects();
            bool again = cc.compose.Compose({ gRed }, { gGreen }, lb2, rb2);
            Check(again, "C17 compose works after over-submission attempts");
            cc.compose.Shutdown();
        }
    }

    // ---- C18: per-eye degenerate policy ----
    {
        ComposeCase cc = MakeComposer(512, 256);
        Check(cc.ready, "C18 composer setup");
        if (cc.ready) {
            // LEFT degenerate (zero-width window), RIGHT valid: the layer's
            // valid half must still compose and the degenerate half must
            // neither smear (the startup bug) nor black out — lower layers
            // (here: the render clear) show through it.
            bool composed = cc.compose.Compose(
                { gRed }, { gGreen },
                { LayerRect{ 0.4f, 0.4f, 0.4f, 0.6f } },   // left: zero width
                { LayerRect{ 0, 0, 1, 1 } });
            const uint8_t* nv12 = nullptr;
            size_t sz = 0;
            bool got = composed && cc.compose.ToNv12(&nv12, &sz);
            Check(got, "C18 compose runs with a one-sided degenerate layer");
            if (got && nv12) {
                bool leftBlack = true, rightGreen = true;
                for (uint32_t y = 8; y < 256; y += 16) {
                    float yl = (float)nv12[(size_t)y * 512 + 64];
                    if (yl < 12.0f || yl > 20.0f) leftBlack = false;
                    float yr = (float)nv12[(size_t)y * 512 + 448];
                    if (yr < 140.6f || yr > 148.6f) rightGreen = false;
                }
                Check(leftBlack, "C18 degenerate LEFT half stays cleared (no texel-column smear)");
                Check(rightGreen, "C18 valid RIGHT half still composes (green)");
            }
            // Both-degenerate: already covered by the smoke suite; repeat here
            // as the policy anchor for the skip rule.
            auto bd = cc.compose.Compose(
                { gRed }, { gGreen },
                { LayerRect{ 0, 0, 0, 0 } }, { LayerRect{ 0, 0, 0, 0 } });
            Check(bd, "C18 both-degenerate layer set still composes (skipped)");
            // Zero-size eye images: rejected WITHOUT disabling the composer
            // (vkCreateImage(0x0) used to Fail() the whole pipeline).
            GpuImage bad = gRed; bad.width = 0; bad.height = 0;
            auto [lb, rb] = fullRects();
            bool zeroSize = cc.compose.Compose({ bad }, { gGreen }, lb, rb);
            Check(!zeroSize, "C18 zero-size eye image rejected");
            Check(cc.compose.IsReady(), "C18 composer STAYS ready after the zero-size attempt");
            auto [lb3, rb3] = fullRects();
            Check(cc.compose.Compose({ gRed }, { gGreen }, lb3, rb3),
                  "C18 compose works after the zero-size attempt");
            cc.compose.Shutdown();
        }
    }

    // ---- C19: subrect bounds + SBS seam ----
    {
        ComposeCase cc = MakeComposer(512, 256);
        Check(cc.ready, "C19 composer setup");
        if (cc.ready) {
            // Gradient eye: red channel ramps 0..255 across the eye width.
            // Window 0.25..0.75 maps to the eye's middle half; inside that
            // window the target shows the gradient's middle slice.
            std::vector<LayerRect> lb{ LayerRect{ 0.25f, 0.0f, 0.75f, 1.0f } };
            std::vector<LayerRect> rb{ LayerRect{ 0.25f, 0.0f, 0.75f, 1.0f } };
            bool composed = cc.compose.Compose({ gGrad }, { gGrad }, lb, rb);
            const uint8_t* nv12 = nullptr;
            size_t sz = 0;
            bool got = composed && cc.compose.ToNv12(&nv12, &sz);
            Check(got, "C19 gradient compose with 0.25..0.75 windows");
            if (got && nv12) {
                // Left rect covers target x 0..255: at x=0 the eye's u=0.25
                // texel (gradient value ~64); at x=255 u=0.75 (~191).
                // BT.601 luma for (v=v, 0, 0): Y = 16 + 65.738*v/255 * ... red
                // luma dominates: Y(v) ~ 16 + 0.257*v(0..255 scale) -> for the
                // ramp compare RELATIVE ordering instead of absolutes.
                float yStart = (float)nv12[(size_t)64 * 512 + 4];
                float yEnd = (float)nv12[(size_t)64 * 512 + 250];
                Check(yEnd > yStart + 15.0f,
                      "C19 left rect shows the gradient ramp (end brighter than start)");
                // Right rect covers x 256..511 with the same ramp.
                float yStartR = (float)nv12[(size_t)64 * 512 + 260];
                float yEndR = (float)nv12[(size_t)64 * 512 + 508];
                Check(yEndR > yStartR + 15.0f, "C19 right rect shows the gradient ramp");
                // Clamped-edge sampling: outside the window the rect borders
                // clamp to the window's edge texel (no wrap, no garbage).
                Check(yStart > 20.0f && yStart < 130.0f, "C19 left edge lands inside the gradient's low end");
                // SBS seam: x=255 belongs to the left rect's last texel, x=256
                // to the right rect's first — both are valid ramp values, and
                // neither side may bleed black across the seam.
                float seamL = (float)nv12[(size_t)64 * 512 + 255];
                float seamR = (float)nv12[(size_t)64 * 512 + 256];
                Check(seamL > 12.0f && seamR > 12.0f, "C19 no black bleed at the SBS middle seam");
            }
            cc.compose.Shutdown();
        }
    }

    // ---- C37: determinism — two composes -> bit-identical NV12 ----
    {
        ComposeCase cc = MakeComposer(512, 256);
        Check(cc.ready, "C37 composer setup");
        if (cc.ready) {
            auto [lb, rb] = fullRects();
            std::vector<uint8_t> first;
            bool okCompose = cc.compose.Compose({ gRed }, { gGreen }, lb, rb);
            const uint8_t* nv12 = nullptr; size_t sz = 0;
            if (okCompose && cc.compose.ToNv12(&nv12, &sz)) first.assign(nv12, nv12 + sz);
            okCompose = cc.compose.Compose({ gRed }, { gGreen }, lb, rb);
            const uint8_t* nv12b = nullptr; size_t szb = 0;
            bool second = okCompose && cc.compose.ToNv12(&nv12b, &szb);
            Check(first.size() == szb && second,
                  "C37 two composes produced same-size NV12");
            if (second && first.size() == szb) {
                Check(memcmp(first.data(), nv12b, szb) == 0,
                      "C37 NV12 bit-identical across identical composes (no lifetime/race artifacts)");
            }
            cc.compose.Shutdown();
        }
    }

    // ---- C20: eye cache (fd churn, prune) ----
    {
        long fdBase = FdCount();
        // 10 frames reusing the same handles: no new imports -> fd count flat.
        ComposeCase cc = MakeComposer(512, 256);
        Check(cc.ready, "C20 composer setup");
        bool all = cc.ready;
        for (int i = 0; i < 10 && all; i++) {
            auto [lb, rb] = fullRects();
            all = cc.compose.Compose({ gRed }, { gGreen }, lb, rb);
        }
        Check(all, "C20 10 frames over the same handles compose");
        long fdAfter10 = FdCount();
        Check(fdAfter10 <= fdBase + 4,
              "C20 no fd-import churn across repeated handles (cache hit path)");
        // Changed geometry, same handle: the recreate path imports afresh.
        GpuImage resized = gRed; resized.width = 64; resized.height = 64;
        {
            auto [lb, rb] = fullRects();
            cc.compose.Compose({ resized }, { gGreen }, lb, rb);
        }
        long fdAfterResize = FdCount();
        Check(fdAfterResize > fdAfter10,
              "C20 changed geometry with the same handle takes the recreate (import) path");
        // Prune: exceed the 16-entry aux cache, compose with two handles, the
        // stale imports must be freed (fd count falls back).
        std::vector<EyeRes> extra;
        for (int i = 0; i < 18; i++) {
            extra.push_back(MakeEye(g_dev, g_phys, g_queue, g_qfam, 32, 32,
                                    (uint8_t)(40 + i), 0, (uint8_t)(200 - i), false));
        }
        bool imported = true;
        for (const auto& e : extra) if (e.fd < 0) imported = false;
        if (imported) {
            long fdFull = FdCount();
            std::vector<GpuImage> gl, gr;
            std::vector<LayerRect> lb, rb;
            gl.push_back(gRed); gr.push_back(gGreen);
            lb.push_back(LayerRect{0,0,1,1}); rb.push_back(LayerRect{0,0,1,1});
            bool ok20 = cc.compose.Compose(gl, gr, lb, rb);
            long fdAfterPrune = FdCount();
            Check(ok20, "C20 compose after cache overfill works");
            Check(fdAfterPrune < fdFull,
                  "C20 prune frees the stale aux imports (fd count drops)");
        } else {
            Check(false, "C20 auxiliary eye imports created");
        }
        for (auto& e : extra) {
            if (e.img) vkDestroyImage(g_dev, e.img, nullptr);
            if (e.mem) vkFreeMemory(g_dev, e.mem, nullptr);
        }
        cc.compose.Shutdown();
    }

    // ---- C21: GPU NV12 vs CPU swscale on the same compose ----
    {
        ComposeCase gpuC = MakeComposer(512, 256);
        ComposeCase oddC = MakeComposer(514, 256);  // dims guard -> CPU path only
        Check(gpuC.ready, "C21 GPU composer setup");
        Check(oddC.ready, "C21 CPU-path composer setup (odd width stays ready)");
        if (gpuC.ready && oddC.ready) {
            auto [lb, rb] = fullRects();
            bool okG = gpuC.compose.Compose({ gGrad }, { gGreen }, lb, rb);
            const uint8_t* nv12G = nullptr; size_t szG = 0;
            bool gotG = okG && gpuC.compose.ToNv12(&nv12G, &szG);
            // The odd-width composer's host copy is the same picture at
            // 514x256; CPU-convert a 512x256 window of it through the same
            // swscale parameters the encoder's fallback uses.
            bool okO = oddC.compose.Compose({ gGrad }, { gGreen }, lb, rb);
            oddC.compose.CopyToHost();
            const uint8_t* host = oddC.compose.HostData();
            Check(gotG && okO && host != nullptr, "C21 GPU NV12 + CPU host copy produced");
            if (gotG && host) {
                // sws BGRA->NV12 with the encoder's exact parameters.
                SwsContext* sws = sws_getContext(512, 256, AV_PIX_FMT_BGRA,
                                                 512, 256, AV_PIX_FMT_NV12,
                                                 SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                Check(sws != nullptr, "C21 swscale context created");
                if (sws) {
                    std::vector<uint8_t> nv12C((size_t)512 * 256 * 3 / 2);
                    const uint8_t* srcSlice[1] = { host };  // 514-wide rows; use per-row slices
                    int srcStride[1] = { 514 * 4 };
                    uint8_t* dstSlice[2] = { nv12C.data(), nv12C.data() + (size_t)512 * 256 };
                    int dstStride[2] = { 512, 512 };
                    int ret = sws_scale(sws, srcSlice, srcStride, 0, 256, dstSlice, dstStride);
                    Check(ret == 256, "C21 CPU swscale converted the host copy");
                    if (ret == 256) {
                        // Compare: mean absolute luma difference must be tiny
                        // (both are BT.601; the GPU path is fixed-point, sws
                        // rounds differently — bit-exactness is NOT the
                        // contract, visible content equality is).
                        long diff = 0;
                        const uint8_t* yG = nv12G;
                        for (size_t i = 0; i < (size_t)512 * 256; i++) diff += abs((int)yG[i] - (int)nv12C[i]);
                        long mean = diff / (512 * 256);
                        Check(mean <= 6, "C21 GPU NV12 matches CPU swscale content (mean luma delta <= 6)");
                    }
                    sws_freeContext(sws);
                }
            }
            gpuC.compose.Shutdown();
            oddC.compose.Shutdown();
        }
    }

    // ---- C38: 100-frame soak (fd-stable, RSS-bounded, zero failures) ----
    {
        ComposeCase cc = MakeComposer(512, 256);
        Check(cc.ready, "C38 composer setup");
        if (cc.ready) {
            size_t rssStart = RssKb();
            long fdStart = FdCount();
            bool allOk = true;
            for (int i = 0; i < 100 && allOk; i++) {
                auto [lb, rb] = fullRects();
                allOk = cc.compose.Compose({ gRed }, { gGreen }, lb, rb);
                const uint8_t* nv12 = nullptr; size_t sz = 0;
                if (allOk) allOk = cc.compose.ToNv12(&nv12, &sz) && nv12 != nullptr;
            }
            Check(allOk, "C38 100 compose+NV12 cycles, zero failed frames");
            size_t rssEnd = RssKb();
            long fdEnd = FdCount();
            Check(rssEnd <= rssStart + 50 * 1024, "C38 RSS growth < 50 MB over the soak");
            Check(fdEnd <= fdStart + 2, "C38 fd count stable over the soak (no import leak)");
            cc.compose.Shutdown();
        }
    }

    // C22 verdict: any validation error is a failure.
    if (vvl) {
        DetachValidationMessenger(g_ctx);
        Check(g_validation.errors.load() == 0, "C22 zero validation errors under KHRONOS validation");
        printf("== (C22 info: %d validation warnings)\n", g_validation.warnings.load());
    }

    printf("== RESULT: %s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
