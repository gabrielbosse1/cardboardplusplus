// TEMP DIAGNOSTIC — runs VulkanCompose's exact pipeline construction headlessly
// on a software Vulkan implementation (lavapipe) and verifies the compose+NV12
// shaders end to end. No OpenVR/D3D dependency: everything VulkanCompose builds
// at Setup/SetupNv12 time is recreated here from the same generated SPIR-V
// headers. Remove before final merge together with the CI step that runs it.
//
// Build (Linux CI): g++ -std=c++17 -I driver_cardboardplusplus/build/spirv \
//   -o vk_smoke tests/vulkan_pipeline_smoke.cpp -lvulkan
// Run with lavapipe: VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json ./vk_smoke
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <vulkan/vulkan.h>

#include "compose_vs.spv.h"
#include "compose_ps.spv.h"
#include "nv12_cs.spv.h"

// Real driver components: phase B links the production translation units
// (not the raw-Vulkan replica above) so the actual Setup/Compose/ToNv12 code
// ships under test, including the degenerate-bounds skip and the NV12 dims
// guard. Phase C (guarded by CBPP_ENCODER_E2E_TEST, wired in CI) runs the
// full VideoEncoder chain on top: compose -> readback -> convert -> FFmpeg
// encode -> Annex-B packets.
#include "VulkanContext.h"
#include "VulkanCompose.h"
#include "openvr_driver.h"
#ifdef CBPP_ENCODER_E2E_TEST
#include "VideoEncoder.h"
#include <mutex>
#endif

#include <unistd.h>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;

static void Check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "== PASS" : "== FAIL", what);
    if (!ok) g_failures++;
}

static void CheckResult(VkResult res, const char* what) {
    printf("== %-42s res=%d%s\n", what, (int)res, res == VK_SUCCESS ? "" : "  <<<<< FAILED");
    if (res != VK_SUCCESS) g_failures++;
}

static uint32_t FindMemType(VkPhysicalDevice phys, uint32_t required, uint32_t propsMask) {
    VkPhysicalDeviceMemoryProperties mp = {};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((required & (1u << i)) && (mp.memoryTypes[i].propertyFlags & propsMask) == propsMask) return i;
    }
    return 0xFFFFFFFFu;
}

struct ImgRes { VkImage img; VkDeviceMemory mem; VkImageView view; };

static ImgRes MakeImage(VkDevice dev, VkPhysicalDevice phys, uint32_t w, uint32_t h,
                        VkImageUsageFlags usage) {
    ImgRes r = {};
    VkImageCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_B8G8R8A8_UNORM;
    ici.extent = { w, h, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCreateImage(dev, &ici, nullptr, &r.img);
    VkMemoryRequirements req = {};
    vkGetImageMemoryRequirements(dev, r.img, &req);
    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = FindMemType(phys, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(dev, &ai, nullptr, &r.mem);
    vkBindImageMemory(dev, r.img, r.mem, 0);
    VkImageViewCreateInfo vci = {};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = r.img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_B8G8R8A8_UNORM;
    vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCreateImageView(dev, &vci, nullptr, &r.view);
    return r;
}

struct BufRes { VkBuffer buf; VkDeviceMemory mem; void* map; };

static BufRes MakeHostBuffer(VkDevice dev, VkPhysicalDevice phys, VkDeviceSize size,
                             VkBufferUsageFlags usage) {
    BufRes r = {};
    VkBufferCreateInfo bci = {};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(dev, &bci, nullptr, &r.buf);
    VkMemoryRequirements req = {};
    vkGetBufferMemoryRequirements(dev, r.buf, &req);
    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = FindMemType(phys, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(dev, &ai, nullptr, &r.mem);
    vkBindBufferMemory(dev, r.buf, r.mem, 0);
    vkMapMemory(dev, r.mem, 0, size, 0, &r.map);
    return r;
}

// ===== Phase B: real driver components (production VulkanContext + VulkanCompose) =====
// The phase above replays the pipeline with a raw-Vulkan replica; this phase
// links the actual production code paths, so whatever ships is what is tested.
//
// Outside SteamVR the OpenVR driver context is a null static pointer, and the
// inline vr::VRDriverLog()/VRSettings()/... accessors dereference it before
// their own null checks can help — the first log call would segfault. Install
// a stub context whose GetGenericInterface always returns null: every accessor
// then resolves to null and the code paths fall back to stderr logging, which
// is exactly the "log host unavailable" state drivers run in at startup.
namespace {
struct StubDriverContext final : vr::IVRDriverContext {
    void* GetGenericInterface(const char*, EVRInitError*) override { return nullptr; }
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
static void RunRealComponentPhase() {
    vr::VRDriverContext() = &g_stubContext;
    printf("== PHASE B: real VulkanContext + VulkanCompose ==\n");

    VulkanContext ctx;
    bool ctxOk = ctx.Initialize(nullptr);  // headless: first device with a graphics queue
    Check(ctxOk, "real VulkanContext::Initialize(nullptr) on lavapipe");
    if (!ctxOk) return;
    VkDevice dev = ctx.Device();
    VkPhysicalDevice phys = ctx.Physical();

    auto getMemFd = (PFN_vkGetMemoryFdKHR)ctx.GetDeviceProc("vkGetMemoryFdKHR");
    Check(getMemFd != nullptr, "vkGetMemoryFdKHR available (external fd export)");
    if (!getMemFd) return;

    // Exportable eye images mirroring the SteamVR IPC image contract: BGRA8
    // with MUTABLE_FORMAT, dedicated allocation, opaque-fd export.
    struct EyeRes {
        VkImage img = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        int fd = -1;
    };
    auto makeEye = [&](uint8_t r, uint8_t g, uint8_t b) -> EyeRes {
        EyeRes e = {};
        VkExternalMemoryImageCreateInfo ext = {};
        ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkImageCreateInfo ici = {};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.pNext = &ext;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_B8G8R8A8_UNORM;
        ici.extent = { 128, 128, 1 };
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ici.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        if (vkCreateImage(dev, &ici, nullptr, &e.img) != VK_SUCCESS) return e;
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
        // Fill the exported memory with a solid color through a staging copy.
        BufRes stage = MakeHostBuffer(dev, phys, (VkDeviceSize)128 * 128 * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        for (int i = 0; i < 128 * 128; i++) {
            uint8_t* px = (uint8_t*)stage.map + (size_t)i * 4;
            px[0] = b; px[1] = g; px[2] = r; px[3] = 255;  // BGRA
        }
        VkCommandPoolCreateInfo pci = {};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.queueFamilyIndex = ctx.GetQueueFamily();
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
        imb.srcAccessMask = 0;
        imb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        imb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        imb.image = e.img;
        imb.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &imb);
        VkBufferImageCopy region = {};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { 128, 128, 1 };
        vkCmdCopyBufferToImage(cmd, stage.buf, e.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkQueueSubmit(ctx.Queue(), 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(ctx.Queue());
        vkDestroyCommandPool(dev, pool, nullptr);
        vkDestroyBuffer(dev, stage.buf, nullptr);
        vkFreeMemory(dev, stage.mem, nullptr);
        // Export the fd (ownership transferred; dup'd later by the importer).
        e.fd = -1;
        VkMemoryGetFdInfoKHR fdInfo = {};
        fdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
        fdInfo.memory = e.mem;
        fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        if (getMemFd(dev, &fdInfo, &e.fd) != VK_SUCCESS)
            e.fd = -1;
        return e;
    };

    printf("[B] makeEye left\n");
    EyeRes eyeL = makeEye(255, 0, 0);    // red, BGRA bytes set as (r,g,b)
    printf("[B] makeEye right\n");
    EyeRes eyeR = makeEye(0, 255, 0);    // green
    Check(eyeL.img != VK_NULL_HANDLE && eyeL.fd >= 0, "exportable left eye created + fd exported");
    Check(eyeR.img != VK_NULL_HANDLE && eyeR.fd >= 0, "exportable right eye created + fd exported");
    if (eyeL.fd < 0 || eyeR.fd < 0) return;

    VulkanCompose compose;
    compose.Init(&ctx);
    Check(compose.Setup(512, 256), "real VulkanCompose::Setup(512,256)");
    if (!compose.IsReady()) return;

    GpuImage gl = {}; gl.handle = (uint64_t)eyeL.fd; gl.width = 128; gl.height = 128;
    gl.format = (uint32_t)VK_FORMAT_B8G8R8A8_UNORM;
    GpuImage gr = gl; gr.handle = (uint64_t)eyeR.fd;
    printf("[B] compose.Compose\n");
    bool composed = compose.Compose({ gl }, { gr },
                                    { LayerRect{ 0, 0, 1, 1 } }, { LayerRect{ 0, 0, 1, 1 } });
    Check(composed, "real VulkanCompose::Compose (production fd import + draw)");
    if (!composed) return;

    printf("[B] ToNv12\n");
    const uint8_t* nv12 = nullptr;
    size_t nv12Size = 0;
    bool nv12Ok = compose.ToNv12(&nv12, &nv12Size);
    Check(nv12Ok && nv12 != nullptr, "real VulkanCompose::ToNv12 (production compute pass)");
    if (nv12Ok) {
        // Same BT.601 oracle as the replica phase: red left / green right.
        float yL = (float)nv12[16 * 512 + 16];
        float yR = (float)nv12[16 * 512 + 480];
        Check(yL > 77.5f && yL < 85.5f, "phase B Y(16,16) red ~81.5");
        Check(yR > 140.6f && yR < 148.6f, "phase B Y(480,16) green ~144.6");
    }

    printf("[B] CopyToHost\n");
    compose.CopyToHost();
    const uint8_t* host = compose.HostData();
    Check(host != nullptr, "real VulkanCompose::CopyToHost + HostData");
    if (host) {
        // BGRA corners: bottom-left red, bottom-right green (top-down rows).
        const uint8_t* bl = host + ((size_t)255 * 512 + 16) * 4;
        const uint8_t* br = host + ((size_t)255 * 512 + 496) * 4;
        Check(bl[2] > 200 && bl[1] < 60 && br[1] > 200 && br[2] < 60,
              "phase B host copy corners: left red / right green");
    }

    // Degenerate-bounds regression: the startup smear. A layer with zero-area
    // rects must be skipped, leaving the cleared target (Y ~ 16 for BT.601
    // black) instead of a stretched texel row.
    printf("[B] degenerate compose\n");
    bool composedDeg = compose.Compose({ gl }, { gr },
                                       { LayerRect{ 0, 0, 0, 0 } }, { LayerRect{ 0, 0, 0, 0 } });
    const uint8_t* nv12d = nullptr;
    size_t nv12dSize = 0;
    bool nv12dOk = composedDeg && compose.ToNv12(&nv12d, &nv12dSize);
    Check(nv12dOk && nv12d != nullptr, "compose accepts degenerate-only layer set");
    if (nv12dOk) {
        bool flatBlack = true;
        for (uint32_t yy = 8; yy < 256 && flatBlack; yy += 24) {
            for (uint32_t xx = 8; xx < 512 && flatBlack; xx += 24) {
                float y = (float)nv12d[yy * 512 + xx];
                if (y < 12.0f || y > 20.0f) flatBlack = false;
            }
        }
        Check(flatBlack, "degenerate layer skipped: target stays cleared (no smear)");
    }

    // NV12 dims guard: the uint-packed compute path needs w%4==0 and h%2==0;
    // Setup must stay ready for compose but refuse the GPU NV12 conversion.
    printf("[B] odd-dims compose\n");
    VulkanCompose odd;
    odd.Init(&ctx);
    odd.Setup(514, 256);  // 514 % 4 == 2
    Check(odd.IsReady(), "odd-width compose stays ready (CPU fallback path)");
    const uint8_t* nv12Odd = nullptr;
    size_t nv12OddSize = 0;
    Check(!odd.ToNv12(&nv12Odd, &nv12OddSize),
          "odd-width NV12 compute refused by the dims guard (no row drift)");

    // Phase C: the full production encode chain (CI-only, links FFmpeg).
#ifdef CBPP_ENCODER_E2E_TEST
    printf("== PHASE C: real VideoEncoder encode chain ==\n");
    VideoEncoder enc;
    printf("[C] encoder Initialize\n");
    bool encOk = enc.Initialize(512, 256, 30, 4000000, false, &ctx, "libx264");
    Check(encOk, "VideoEncoder::Initialize (lavapipe compose + libx264)");
    if (encOk) {
        std::mutex pktLock;
        std::vector<std::vector<uint8_t>> packets;
        std::vector<bool> keyflags;
        enc.SetEncodedPacketCallback([&](uint8_t* d, int s, int64_t, bool kf) {
            std::lock_guard<std::mutex> l(pktLock);
            packets.emplace_back(d, d + s);
            keyflags.push_back(kf);
        });
        printf("[C] ComposeSBSGPU\n");
        Check(enc.ComposeSBSGPU({ gl }, { gr },
                                { VideoEncoder::LayerBounds{ 0, 0, 1, 1 } },
                                { VideoEncoder::LayerBounds{ 0, 0, 1, 1 } }),
              "encoder ComposeSBSGPU over imported fd eyes");
        printf("[C] Readback\n");
        Check(enc.ReadbackToBuffer(), "encoder ReadbackToBuffer (GPU NV12 or CPU fallback)");
        printf("[C] Convert\n");
        Check(enc.ConvertFrame(), "encoder ConvertFrame");
        printf("[C] FinishEncode\n");
        Check(enc.FinishEncode(0), "encoder FinishEncode(0)");
        // Idle heartbeat path: re-encode the last produced frame without a
        // new compose; keeps late joiners decodable while SteamVR idles.
        printf("[C] heartbeat\n");
        Check(enc.ReencodeLastFrame(1), "ReencodeLastFrame(1) idle heartbeat");

        size_t nPkts = 0;
        bool sawIdr = false, sawNonIdr = false, annexb = false;
        {
            std::lock_guard<std::mutex> l(pktLock);
            nPkts = packets.size();
            for (const auto& p : packets) {
                if (p.size() >= 5 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) annexb = true;
                for (size_t i = 0; i + 4 < p.size(); i++) {
                    if (p[i] == 0 && p[i + 1] == 0 && (p[i + 2] == 1 || (p[i + 2] == 0 && p[i + 3] == 1))) {
                        size_t nal = i + (p[i + 2] == 1 ? 3 : 4);
                        uint8_t t = (uint8_t)(p[nal] & 0x1F);
                        if (t == 5) sawIdr = true;
                        if (t == 1 || t == 7 || t == 8) sawNonIdr = true;
                    }
                }
            }
        }
        Check(nPkts >= 2, "encode chain produced >=2 packets (frame + heartbeat)");
        Check(annexb, "packets are Annex-B (start codes present)");
        Check(sawIdr, "first frame encoded an IDR");
        Check(sawNonIdr, "SPS/PPS/slice NALs present");
    }
#endif
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);  // survive crashes: no lost buffer
    printf("== SPIR-V words: vs=%zu ps=%zu cs=%zu\n",
           sizeof(CBPPSpirv_compose_vs) / 4, sizeof(CBPPSpirv_compose_ps) / 4,
           sizeof(CBPPSpirv_nv12_cs) / 4);
    Check(CBPPSpirv_compose_vs[0] == 0x07230203, "compose_vs SPIR-V magic");
    Check(CBPPSpirv_compose_ps[0] == 0x07230203, "compose_ps SPIR-V magic");
    Check(CBPPSpirv_nv12_cs[0] == 0x07230203, "nv12_cs SPIR-V magic");

    VkApplicationInfo app = {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_MAKE_API_VERSION(0, 1, 3, 0);
    VkInstanceCreateInfo ici2 = {};
    ici2.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici2.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    VkResult res = vkCreateInstance(&ici2, nullptr, &inst);
    CheckResult(res, "vkCreateInstance");
    if (res != VK_SUCCESS) return 1;

    uint32_t ndev = 0;
    vkEnumeratePhysicalDevices(inst, &ndev, nullptr);
    std::vector<VkPhysicalDevice> phys(ndev);
    vkEnumeratePhysicalDevices(inst, &ndev, phys.data());
    if (ndev == 0) { printf("== FAIL no physical devices\n"); return 1; }
    VkPhysicalDevice gpu = phys[0];
    VkPhysicalDeviceProperties props = {};
    vkGetPhysicalDeviceProperties(gpu, &props);
    printf("== device: %s api=0x%x\n", props.deviceName, props.apiVersion);

    uint32_t qfam = 0xFFFFFFFFu;
    uint32_t nfam = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nfam, nullptr);
    std::vector<VkQueueFamilyProperties> fams(nfam);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nfam, fams.data());
    for (uint32_t i = 0; i < nfam; i++) {
        if ((fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (fams[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            qfam = i; break;
        }
    }
    if (qfam == 0xFFFFFFFFu) { printf("== FAIL no graphics+compute queue\n"); return 1; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = {};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice dev = VK_NULL_HANDLE;
    res = vkCreateDevice(gpu, &dci, nullptr, &dev);
    CheckResult(res, "vkCreateDevice");
    if (res != VK_SUCCESS) return 1;
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(dev, qfam, 0, &queue);

    // Descriptor set layouts — mirror VulkanCompose::Setup / SetupNv12 exactly.
    VkDescriptorSetLayoutBinding gb[4] = {};
    gb[0] = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    gb[1] = { 1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    gb[2] = { 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    gb[3] = { 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo glci = {};
    glci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    glci.bindingCount = 4;
    glci.pBindings = gb;
    VkDescriptorSetLayout gfxLayout = VK_NULL_HANDLE;
    CheckResult(vkCreateDescriptorSetLayout(dev, &glci, nullptr, &gfxLayout), "vkCreateDescriptorSetLayout(gfx)");

    VkDescriptorSetLayoutBinding cb[3] = {};
    cb[0] = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    cb[1] = { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    cb[2] = { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo clci = {};
    clci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    clci.bindingCount = 3;
    clci.pBindings = cb;
    VkDescriptorSetLayout csLayout = VK_NULL_HANDLE;
    CheckResult(vkCreateDescriptorSetLayout(dev, &clci, nullptr, &csLayout), "vkCreateDescriptorSetLayout(cs)");

    VkPipelineLayoutCreateInfo plci = {};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &gfxLayout;
    VkPipelineLayout gfxPipeLayout = VK_NULL_HANDLE;
    CheckResult(vkCreatePipelineLayout(dev, &plci, nullptr, &gfxPipeLayout), "vkCreatePipelineLayout(gfx)");
    VkPipelineLayoutCreateInfo cplci = {};
    cplci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    cplci.setLayoutCount = 1;
    cplci.pSetLayouts = &csLayout;
    VkPipelineLayout csPipeLayout = VK_NULL_HANDLE;
    CheckResult(vkCreatePipelineLayout(dev, &cplci, nullptr, &csPipeLayout), "vkCreatePipelineLayout(cs)");

    // Render pass — mirror VulkanCompose::Setup exactly.
    VkAttachmentDescription ad = {};
    ad.format = VK_FORMAT_B8G8R8A8_UNORM;
    ad.samples = VK_SAMPLE_COUNT_1_BIT;
    ad.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ad.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ad.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ar;
    VkSubpassDependency dep = {};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dep.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpci = {};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &ad;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dep;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    CheckResult(vkCreateRenderPass(dev, &rpci, nullptr, &renderPass), "vkCreateRenderPass");

    // Shader modules from the same generated headers the driver embeds.
    struct ModInfo { const char* name; const uint32_t* words; size_t count; };
    ModInfo mods[3] = {
        { "compose_vs", CBPPSpirv_compose_vs, sizeof(CBPPSpirv_compose_vs) / 4 },
        { "compose_ps", CBPPSpirv_compose_ps, sizeof(CBPPSpirv_compose_ps) / 4 },
        { "nv12_cs", CBPPSpirv_nv12_cs, sizeof(CBPPSpirv_nv12_cs) / 4 },
    };
    VkShaderModule sm[3] = {};
    for (int i = 0; i < 3; i++) {
        VkShaderModuleCreateInfo smci = {};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = mods[i].count * 4;
        smci.pCode = mods[i].words;
        char label[64];
        snprintf(label, sizeof(label), "vkCreateShaderModule(%s)", mods[i].name);
        CheckResult(vkCreateShaderModule(dev, &smci, nullptr, &sm[i]), label);
    }

    // Graphics pipelines (opaque + alpha) — mirror Setup exactly.
    VkPipeline gfxPipes[2] = {};
    for (int i = 0; i < 2; i++) {
        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = sm[0];
        stages[0].pName = "main";
        stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = sm[1];
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi = {};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia = {};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp = {};
        vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        VkViewport viewport = { 0.0f, 0.0f, 512.0f, 256.0f, 0.0f, 1.0f };
        VkRect2D scissor = { { 0, 0 }, { 512, 256 } };
        vp.viewportCount = 1;
        vp.pViewports = &viewport;
        vp.scissorCount = 1;
        vp.pScissors = &scissor;
        VkPipelineRasterizationStateCreateInfo rs = {};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms = {};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds = {};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        VkPipelineColorBlendAttachmentState cba = {};
        cba.colorWriteMask = 0xF;
        if (i == 1) {
            cba.blendEnable = VK_TRUE;
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        }
        VkPipelineColorBlendStateCreateInfo cbl = {};
        cbl.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cbl.attachmentCount = 1;
        cbl.pAttachments = &cba;
        VkGraphicsPipelineCreateInfo gpci = {};
        gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gpci.stageCount = 2;
        gpci.pStages = stages;
        gpci.pVertexInputState = &vi;
        gpci.pInputAssemblyState = &ia;
        gpci.pViewportState = &vp;
        gpci.pRasterizationState = &rs;
        gpci.pMultisampleState = &ms;
        gpci.pDepthStencilState = &ds;
        gpci.pColorBlendState = &cbl;
        gpci.layout = gfxPipeLayout;
        gpci.renderPass = renderPass;
        char label[48];
        snprintf(label, sizeof(label), "vkCreateGraphicsPipelines(%s)", i == 0 ? "opaque" : "alpha");
        CheckResult(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, nullptr, &gfxPipes[i]), label);
    }

    // Compute pipeline — mirror SetupNv12 exactly.
    VkComputePipelineCreateInfo cpci = {};
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = sm[2];
    cpci.stage.pName = "main";
    cpci.layout = csPipeLayout;
    VkPipeline csPipe = VK_NULL_HANDLE;
    CheckResult(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &csPipe),
                "vkCreateComputePipelines");

    // Resources for the functional test: 512x256 target, 128x128 red / green eyes.
    const uint32_t W = 512, H = 256, EW = 128, EH = 128;
    ImgRes native = MakeImage(dev, gpu, W, H,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    ImgRes eyeL = MakeImage(dev, gpu, EW, EH, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    ImgRes eyeR = MakeImage(dev, gpu, EW, EH, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    Check(native.img != VK_NULL_HANDLE && eyeL.img != VK_NULL_HANDLE && eyeR.img != VK_NULL_HANDLE,
          "images created");

    BufRes staging = MakeHostBuffer(dev, gpu, (VkDeviceSize)EW * EH * 4 * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    BufRes bounds = MakeHostBuffer(dev, gpu, 32, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    BufRes params = MakeHostBuffer(dev, gpu, 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    BufRes nv12 = MakeHostBuffer(dev, gpu, (VkDeviceSize)W * H * 3 / 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    uint8_t* red = (uint8_t*)staging.map;
    uint8_t* green = red + (size_t)EW * EH * 4;
    for (int i = 0; i < EW * EH; i++) {
        red[4 * i + 0] = 0;   red[4 * i + 1] = 0;   red[4 * i + 2] = 255; red[4 * i + 3] = 255;
        green[4 * i + 0] = 0; green[4 * i + 1] = 255; green[4 * i + 2] = 0; green[4 * i + 3] = 255;
    }
    float* b = (float*)bounds.map;
    b[0] = 0; b[1] = 0; b[2] = 1; b[3] = 1;   // left rect: x, y, w, h
    b[4] = 0; b[5] = 0; b[6] = 1; b[7] = 1;   // right rect
    int32_t* dims = (int32_t*)params.map;
    dims[0] = (int32_t)W; dims[1] = (int32_t)H; dims[2] = (int32_t)(W * H); dims[3] = 0;
    memset(nv12.map, 0xAB, (size_t)W * H * 3 / 2); // poison: any unwritten byte is visible

    VkSamplerCreateInfo sci = {};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 1.0f;
    VkSampler sampler = VK_NULL_HANDLE;
    CheckResult(vkCreateSampler(dev, &sci, nullptr, &sampler), "vkCreateSampler");

    VkDescriptorPoolSize sizes[4] = {};
    sizes[0] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4 };
    sizes[1] = { VK_DESCRIPTOR_TYPE_SAMPLER, 2 };
    sizes[2] = { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8 };
    sizes[3] = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 };
    VkDescriptorPoolCreateInfo dpci = {};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 4;
    dpci.poolSizeCount = 4;
    dpci.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    CheckResult(vkCreateDescriptorPool(dev, &dpci, nullptr, &pool), "vkCreateDescriptorPool");

    VkDescriptorSetLayout layouts[2] = { gfxLayout, csLayout };
    VkDescriptorSetAllocateInfo dsai = {};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = pool;
    dsai.descriptorSetCount = 2;
    dsai.pSetLayouts = layouts;
    VkDescriptorSet sets[2] = {};
    CheckResult(vkAllocateDescriptorSets(dev, &dsai, sets), "vkAllocateDescriptorSets");

    VkDescriptorImageInfo sampInfo = {};
    sampInfo.sampler = sampler;
    VkDescriptorImageInfo eyeInfo[2] = {};
    eyeInfo[0].imageView = eyeL.view;
    eyeInfo[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    eyeInfo[1].imageView = eyeR.view;
    eyeInfo[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorBufferInfo boundsInfo = { bounds.buf, 0, 32 };
    VkWriteDescriptorSet gw[4] = {};
    for (int i = 0; i < 4; i++) gw[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    gw[0].dstSet = sets[0]; gw[0].dstBinding = 0; gw[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    gw[0].descriptorCount = 1; gw[0].pBufferInfo = &boundsInfo;
    gw[1].dstSet = sets[0]; gw[1].dstBinding = 1; gw[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    gw[1].descriptorCount = 1; gw[1].pImageInfo = &sampInfo;
    gw[2].dstSet = sets[0]; gw[2].dstBinding = 2; gw[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    gw[2].descriptorCount = 1; gw[2].pImageInfo = &eyeInfo[0];
    gw[3].dstSet = sets[0]; gw[3].dstBinding = 3; gw[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    gw[3].descriptorCount = 1; gw[3].pImageInfo = &eyeInfo[1];
    vkUpdateDescriptorSets(dev, 4, gw, 0, nullptr);

    VkDescriptorImageInfo nativeInfo = {};
    nativeInfo.imageView = native.view;
    nativeInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorBufferInfo paramsInfo = { params.buf, 0, 16 };
    VkDescriptorBufferInfo outInfo = { nv12.buf, 0, W * H * 3 / 2 };
    VkWriteDescriptorSet cw[3] = {};
    for (int i = 0; i < 3; i++) cw[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    cw[0].dstSet = sets[1]; cw[0].dstBinding = 0; cw[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    cw[0].descriptorCount = 1; cw[0].pBufferInfo = &paramsInfo;
    cw[1].dstSet = sets[1]; cw[1].dstBinding = 1; cw[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    cw[1].descriptorCount = 1; cw[1].pImageInfo = &nativeInfo;
    cw[2].dstSet = sets[1]; cw[2].dstBinding = 2; cw[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    cw[2].descriptorCount = 1; cw[2].pBufferInfo = &outInfo;
    vkUpdateDescriptorSets(dev, 3, cw, 0, nullptr);

    VkFramebufferCreateInfo fbci = {};
    fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbci.renderPass = renderPass;
    fbci.attachmentCount = 1;
    fbci.pAttachments = &native.view;
    fbci.width = W;
    fbci.height = H;
    fbci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    CheckResult(vkCreateFramebuffer(dev, &fbci, nullptr, &fb), "vkCreateFramebuffer");

    VkCommandPoolCreateInfo cpoolci = {};
    cpoolci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpoolci.queueFamilyIndex = qfam;
    VkCommandPool cpool = VK_NULL_HANDLE;
    vkCreateCommandPool(dev, &cpoolci, nullptr, &cpool);
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbai = {};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = cpool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    vkAllocateCommandBuffers(dev, &cbai, &cmd);
    VkFenceCreateInfo fci = {};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(dev, &fci, nullptr, &fence);

    // Cmd 1: upload eye textures, compose.
    VkCommandBufferBeginInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkBufferImageCopy cpyL = {};
    cpyL.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    cpyL.imageExtent = { EW, EH, 1 };
    VkBufferImageCopy cpyR = cpyL;
    cpyR.bufferOffset = (VkDeviceSize)EW * EH * 4;
    VkImageMemoryBarrier imb = {};
    imb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imb.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    imb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    imb.srcQueueFamilyIndex = imb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imb.image = eyeL.img;
    imb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &imb);
    imb.image = eyeR.img;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &imb);
    vkCmdCopyBufferToImage(cmd, staging.buf, eyeL.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cpyL);
    vkCmdCopyBufferToImage(cmd, staging.buf, eyeR.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cpyR);
    imb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    imb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    imb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    imb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    imb.image = eyeL.img;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &imb);
    imb.image = eyeR.img;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &imb);
    imb.srcAccessMask = 0;
    imb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    imb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imb.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    imb.image = native.img;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &imb);
    VkRenderPassBeginInfo rpi = {};
    rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpi.renderPass = renderPass;
    rpi.framebuffer = fb;
    rpi.renderArea = { { 0, 0 }, { W, H } };
    VkClearValue cv = {};
    rpi.clearValueCount = 1;
    rpi.pClearValues = &cv;
    vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gfxPipes[0]);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gfxPipeLayout, 0, 1, &sets[0], 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkResetFences(dev, 1, &fence);
    res = vkQueueSubmit(queue, 1, &si, fence);
    CheckResult(res, "compose vkQueueSubmit");
    if (res == VK_SUCCESS) {
        res = vkWaitForFences(dev, 1, &fence, VK_TRUE, 10ULL * 1000 * 1000 * 1000);
        CheckResult(res, "compose vkWaitForFences");
    }

    // Cmd 2: BGRA -> NV12 compute pass, exactly like ToNv12.
    vkBeginCommandBuffer(cmd, &bi);
    imb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    imb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    imb.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    imb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imb.image = native.img;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &imb);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, csPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, csPipeLayout, 0, 1, &sets[1], 0, nullptr);
    vkCmdDispatch(cmd, (W / 8 + 7) / 8, (H + 7) / 8, 1);
    imb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    imb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    imb.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imb.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &imb);
    vkEndCommandBuffer(cmd);
    vkResetFences(dev, 1, &fence);
    res = vkQueueSubmit(queue, 1, &si, fence);
    CheckResult(res, "nv12 vkQueueSubmit");
    if (res == VK_SUCCESS) {
        res = vkWaitForFences(dev, 1, &fence, VK_TRUE, 10ULL * 1000 * 1000 * 1000);
        CheckResult(res, "nv12 vkWaitForFences");
    }

    // Verify the NV12 output: left half = red (sRGB), right half = green.
    const uint8_t* yPlane = (const uint8_t*)nv12.map;
    const uint8_t* uvPlane = yPlane + (size_t)W * H;
    struct Sample { uint32_t x, y; float want; const char* what; };
    Sample ys[] = {
        { 16, 16, 81.5f, "Y left red" },
        { 480, 16, 144.6f, "Y right green" },
        { 16, 240, 81.5f, "Y bottom-left" },
        { 480, 240, 144.6f, "Y bottom-right" },
    };
    for (const Sample& s : ys) {
        float got = (float)yPlane[s.y * W + s.x];
        bool ok = got > s.want - 4 && got < s.want + 4;
        printf("== Y(%3u,%3u) got=%5.1f want~%5.1f %s (%s)\n", s.x, s.y, got, s.want,
               ok ? "PASS" : "FAIL", s.what);
        if (!ok) g_failures++;
    }
    struct UvSample { uint32_t bx; float wantU, wantV; const char* what; };
    UvSample uvs[] = {
        { 16, 90.2f, 240.0f, "UV left red" },
        { 480, 53.8f, 34.2f, "UV right green" },
    };
    for (const UvSample& s : uvs) {
        // Chroma sample index = bx/2 (byte x -> chroma x); row = 16/2.
        uint32_t cxi = s.bx / 2;
        const uint8_t* uv = uvPlane + ((16 / 2) * W) + cxi * 2;
        float gotU = (float)uv[0], gotV = (float)uv[1];
        bool ok = gotU > s.wantU - 6 && gotU < s.wantU + 6 && gotV > s.wantV - 6 && gotV < s.wantV + 6;
        printf("== UV(%3u,~) got=(%5.1f,%5.1f) want~(%5.1f,%5.1f) %s (%s)\n", s.bx, gotU, gotV,
               s.wantU, s.wantV, ok ? "PASS" : "FAIL", s.what);
        if (!ok) g_failures++;
    }
    int poisonY = 0, poisonUv = 0;
    for (size_t i = 0; i < (size_t)W * H; i++) if (yPlane[i] == 0xAB) poisonY++;
    for (size_t i = 0; i < (size_t)W * H / 2; i++) if (uvPlane[i] == 0xAB) poisonUv++;
    printf("== unwritten Y bytes=%d uv bytes=%d %s\n", poisonY, poisonUv,
           (poisonY == 0 && poisonUv == 0) ? "PASS" : "FAIL");
    if (poisonY != 0 || poisonUv != 0) g_failures++;

    RunRealComponentPhase();

    printf("== RESULT: %s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
