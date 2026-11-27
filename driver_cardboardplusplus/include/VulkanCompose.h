#pragma once
#include "VulkanContext.h"
#include "GpuImage.h"
#include <vector>
struct LayerRect {
    float uMin = 0.0f, vMin = 0.0f, uMax = 1.0f, vMax = 1.0f;
};

// Vulkan SBS composer: imports the compositor eye images once, draws every
// submitted layer into a Vulkan-native target and converts the result to NV12 on
// the GPU. A setup failure keeps IsReady() false and VideoEncoder drops frames.
class VulkanCompose {
public:
    void Init(VulkanContext* ctx);
    bool Setup(uint32_t width, uint32_t height);
    bool IsReady() const { return m_pCtx && m_pCtx->IsReady() && m_ready; }
    // Converts the composed frame to NV12 in a host-visible buffer, replacing
    // the CPU swscale pass. Returns false when the compute path is unavailable.
    bool ToNv12(const uint8_t** outData, size_t* outSize);
    // Copies the composed frame to host-visible memory. This is the source
    // for the software conversion path and for the frame hash diagnostic.
    void CopyToHost();
    const uint8_t* HostData() const { return m_hostMap; }
    bool Compose(const std::vector<GpuImage>& lefts,
                 const std::vector<GpuImage>& rights,
                 const std::vector<LayerRect>& leftBounds,
                 const std::vector<LayerRect>& rightBounds);
    void Shutdown();
private:
    bool LazyInit();
    bool SetupNv12();
    void ShutdownNv12();
    void Fail(const char* why);
    int FindOrImportEye(const GpuImage& src);
    static const uint32_t kMaxLayers = 4;

    VulkanContext* m_pCtx = nullptr;
    bool m_ready = false;
    bool m_lazyDone = false;
    uint32_t m_width = 0, m_height = 0;
    // Native Vulkan compose target: the render pass and the NV12 compute pass
    // both work on this image, so no cross-API image is touched while composing.
    VkImage m_nativeImage = VK_NULL_HANDLE;
    VkDeviceMemory m_nativeMem = VK_NULL_HANDLE;
    VkImageView m_nativeView = VK_NULL_HANDLE;
    bool m_nativeLayoutReady = false;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkFramebuffer m_framebuffer = VK_NULL_HANDLE;
    VkPipeline m_pipelineOpaque = VK_NULL_HANDLE;
    VkPipeline m_pipelineAlpha = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descSets[kMaxLayers] = {};
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkCommandPool m_cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
    VkFence m_fence = VK_NULL_HANDLE;

    struct BoundsBuf {
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        float* map = nullptr;
    };
    BoundsBuf m_bounds[kMaxLayers];

    // Imported eye images cached by handle, extent and format; a layer-copy
    // recreate reuses the geometry but changes the handle, so both key the entry.
    struct AuxImg {
        uint64_t handle = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t srcFormat = 0;
        VkImage img = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFormat fmt = VK_FORMAT_B8G8R8A8_UNORM;
        bool layoutReady = false;   // UNDED->TRANSFER_SRC barrier applied once
        bool keyed = false;         // source exposes a cross-API sync primitive (Windows sync object, Linux external semaphore)
    };
    std::vector<AuxImg> m_aux;
    // Keeps only the eyes used by the frame just drawn; every entry holds a
    // full-resolution GPU image, so an unbounded cache leaks megabytes per
    // layer-copy recreate.
    void PruneAux(const int* used, size_t usedCount);

    uint64_t m_frame = 0;
    uint32_t m_submitFails = 0;

    // Host-visible copy of the composed frame (software conversion source).
    VkBuffer m_hostBuf = VK_NULL_HANDLE;
    VkDeviceMemory m_hostMem = VK_NULL_HANDLE;
    uint8_t* m_hostMap = nullptr;
    size_t m_hostSize = 0;
    uint64_t m_hostFrame = 0;
    uint64_t m_hostChanges = 0;
    uint32_t m_hostHash = 0;

    // BGRA -> NV12 compute path, replacing the CPU swscale conversion.
    VkShaderModule m_csMod = VK_NULL_HANDLE;
    VkPipeline m_csPipe = VK_NULL_HANDLE;
    VkPipelineLayout m_csPipeLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_csSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_csPool = VK_NULL_HANDLE;
    VkDescriptorSet m_csSet = VK_NULL_HANDLE;
    VkBuffer m_csParams = VK_NULL_HANDLE;
    VkDeviceMemory m_csParamsMem = VK_NULL_HANDLE;
    VkBuffer m_nv12Buf = VK_NULL_HANDLE;
    VkDeviceMemory m_nv12Mem = VK_NULL_HANDLE;
    uint8_t* m_nv12Map = nullptr;
    size_t m_nv12Size = 0;
    bool m_nv12Ready = false;
};
