#include "VulkanCompose.h"
#include "DriverLog.h"
#include <cstdio>
#include <vector>
#include <cstring>
#ifdef _WIN32
#include "SpirvCompiler.h"
#include <d3dcompiler.h>
#include <dxgi.h>
#pragma comment(lib, "d3dcompiler.lib")
#else
#include "compose_vs.spv.h"
#include "compose_ps.spv.h"
#include "nv12_cs.spv.h"
#endif
#pragma comment(lib, "d3dcompiler.lib")

// Vulkan gets y-up NDC; the software complement multiplies y by -2.
#include "shaders/compose_vs.hlsli"
#include "shaders/compose_ps.hlsli"
#include "shaders/nv12_cs.hlsli"


#ifdef _WIN32
static VkFormat HostFormatToVk(int hostFormat) {
    switch (hostFormat) {
        case 87: return VK_FORMAT_B8G8R8A8_UNORM;
        case 88: return VK_FORMAT_B8G8R8A8_SRGB;
        case 28: return VK_FORMAT_R8G8B8A8_UNORM;
        case 29: return VK_FORMAT_R8G8B8A8_SRGB;
        case 91: return VK_FORMAT_R8_UNORM;
        case 24: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case 54: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case 56: return VK_FORMAT_R16G16_SFLOAT;
        case 61: return VK_FORMAT_R16_SFLOAT;
        case 45: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case 10: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case 95: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        default: return VK_FORMAT_UNDEFINED;
    }
}
#endif

void VulkanCompose::Init(VulkanContext* ctx) {
    m_pCtx = ctx;
}

void VulkanCompose::Fail(const char* why) {
    DriverLog("VK-COMPOSE disabled: %s", why);
    m_ready = false;
}

void VulkanCompose::Shutdown() {
    m_ready = false;
    if (!m_pCtx || !m_pCtx->IsReady()) { m_pCtx = nullptr; return; }
    ShutdownNv12();
    auto* d = m_pCtx->Device();
    auto freeMem0 = (PFN_vkFreeMemory)m_pCtx->GetDeviceProc("vkFreeMemory");
    auto destroyImgView0 = (PFN_vkDestroyImageView)m_pCtx->GetDeviceProc("vkDestroyImageView");
    auto destroyImg0 = (PFN_vkDestroyImage)m_pCtx->GetDeviceProc("vkDestroyImage");
    if (m_nativeView && destroyImgView0) { destroyImgView0(d, m_nativeView, nullptr); m_nativeView = VK_NULL_HANDLE; }
    if (m_nativeImage && destroyImg0) { destroyImg0(d, m_nativeImage, nullptr); m_nativeImage = VK_NULL_HANDLE; }
    if (m_nativeMem && freeMem0) { freeMem0(d, m_nativeMem, nullptr); m_nativeMem = VK_NULL_HANDLE; }
    m_nativeLayoutReady = false;
    auto freeMem = (PFN_vkFreeMemory)m_pCtx->GetDeviceProc("vkFreeMemory");
    auto destroyBuf = (PFN_vkDestroyBuffer)m_pCtx->GetDeviceProc("vkDestroyBuffer");
    auto destroyImgView = (PFN_vkDestroyImageView)m_pCtx->GetDeviceProc("vkDestroyImageView");
    auto destroyImg = (PFN_vkDestroyImage)m_pCtx->GetDeviceProc("vkDestroyImage");
    auto destroyFb = (PFN_vkDestroyFramebuffer)m_pCtx->GetDeviceProc("vkDestroyFramebuffer");
    auto destroyRp = (PFN_vkDestroyRenderPass)m_pCtx->GetDeviceProc("vkDestroyRenderPass");
    auto destroyPipe = (PFN_vkDestroyPipeline)m_pCtx->GetDeviceProc("vkDestroyPipeline");
    auto destroyLayout = (PFN_vkDestroyPipelineLayout)m_pCtx->GetDeviceProc("vkDestroyPipelineLayout");
    auto destroySetLayout = (PFN_vkDestroyDescriptorSetLayout)m_pCtx->GetDeviceProc("vkDestroyDescriptorSetLayout");
    auto destroyPool = (PFN_vkDestroyDescriptorPool)m_pCtx->GetDeviceProc("vkDestroyDescriptorPool");
    auto destroySampler = (PFN_vkDestroySampler)m_pCtx->GetDeviceProc("vkDestroySampler");
    auto destroyPool2 = (PFN_vkDestroyCommandPool)m_pCtx->GetDeviceProc("vkDestroyCommandPool");
    auto destroyFence = (PFN_vkDestroyFence)m_pCtx->GetDeviceProc("vkDestroyFence");
    for (int i = 0; i < (int)kMaxLayers; i++) {
        if (m_bounds[i].map) {
            auto unmap = (PFN_vkUnmapMemory)m_pCtx->GetDeviceProc("vkUnmapMemory");
            if (unmap) unmap(d, m_bounds[i].mem);
            m_bounds[i].map = nullptr;
        }
        if (m_bounds[i].buf && destroyBuf) destroyBuf(d, m_bounds[i].buf, nullptr);
        if (m_bounds[i].mem && freeMem) freeMem(d, m_bounds[i].mem, nullptr);
        m_bounds[i] = {};
    }
    for (auto& a : m_aux) {
        if (a.view && destroyImgView) destroyImgView(d, a.view, nullptr);
        if (a.img && destroyImg) destroyImg(d, a.img, nullptr);
        if (a.mem && freeMem) freeMem(d, a.mem, nullptr);
    }
    m_aux.clear();
    if (m_framebuffer && destroyFb) destroyFb(d, m_framebuffer, nullptr);
    if (m_renderPass && destroyRp) destroyRp(d, m_renderPass, nullptr);
    if (m_pipelineOpaque && destroyPipe) destroyPipe(d, m_pipelineOpaque, nullptr);
    if (m_pipelineAlpha && destroyPipe) destroyPipe(d, m_pipelineAlpha, nullptr);
    if (m_pipelineLayout && destroyLayout) destroyLayout(d, m_pipelineLayout, nullptr);
    if (m_setLayout && destroySetLayout) destroySetLayout(d, m_setLayout, nullptr);
    if (m_descPool && destroyPool) destroyPool(d, m_descPool, nullptr);
    if (m_sampler && destroySampler) destroySampler(d, m_sampler, nullptr);
    if (m_fence && destroyFence) destroyFence(d, m_fence, nullptr);
    if (m_cmdPool && destroyPool2) destroyPool2(d, m_cmdPool, nullptr);
    m_framebuffer = VK_NULL_HANDLE;
    m_renderPass = VK_NULL_HANDLE;
    m_pipelineOpaque = VK_NULL_HANDLE;
    m_pipelineAlpha = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_setLayout = VK_NULL_HANDLE;
    m_descPool = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
    m_fence = VK_NULL_HANDLE;
    m_cmdPool = VK_NULL_HANDLE;
    m_cmd = VK_NULL_HANDLE;
    m_pCtx = nullptr;
}

// Vulkan-side pipeline setup. Descriptor layout matches the dxbc-spirv
// BasicResourceMapping output for the SBS PS exactly:
//   0 = cbuffer (uniform buffer), 1 = sampler, 2 = t0 sampled image, 3 = t1 sampled image.
// The composer owns its Vulkan-native target; nothing leaves Vulkan except the eye images imported from the compositor.
static bool PickCoherentHostType(const VkMemoryRequirements& req,
                                 VkPhysicalDevice phys,
                                 PFN_vkGetPhysicalDeviceMemoryProperties getMemProps,
                                 uint32_t* outType);

bool VulkanCompose::Setup(uint32_t width, uint32_t height) {
    if (!m_pCtx || !m_pCtx->IsReady()) return false;
    m_width = width;
    m_height = height;

    if (!LazyInit()) return false;

    auto createShaderModule = (PFN_vkCreateShaderModule)m_pCtx->GetDeviceProc("vkCreateShaderModule");
    auto createSetLayout = (PFN_vkCreateDescriptorSetLayout)m_pCtx->GetDeviceProc("vkCreateDescriptorSetLayout");
    auto createPool = (PFN_vkCreateDescriptorPool)m_pCtx->GetDeviceProc("vkCreateDescriptorPool");
    auto allocSets = (PFN_vkAllocateDescriptorSets)m_pCtx->GetDeviceProc("vkAllocateDescriptorSets");
    auto createPipeLayout = (PFN_vkCreatePipelineLayout)m_pCtx->GetDeviceProc("vkCreatePipelineLayout");
    auto createRenderPass = (PFN_vkCreateRenderPass)m_pCtx->GetDeviceProc("vkCreateRenderPass");
    auto createFramebuffer = (PFN_vkCreateFramebuffer)m_pCtx->GetDeviceProc("vkCreateFramebuffer");
    auto createPipeline = (PFN_vkCreateGraphicsPipelines)m_pCtx->GetDeviceProc("vkCreateGraphicsPipelines");
    auto createImgView = (PFN_vkCreateImageView)m_pCtx->GetDeviceProc("vkCreateImageView");
    auto createSampler = (PFN_vkCreateSampler)m_pCtx->GetDeviceProc("vkCreateSampler");
    if (!createShaderModule || !createSetLayout || !createPool || !allocSets || !createPipeLayout ||
        !createRenderPass || !createFramebuffer || !createPipeline || !createImgView || !createSampler) {
        Fail("setup commands missing");
        return false;
    }
    // Descriptor set layout, in the exact order BasicResourceMapping emits.
    VkDescriptorSetLayoutBinding bindings[4] = {};
    bindings[0] = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    bindings[1] = { 1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    bindings[2] = { 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    bindings[3] = { 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo slci = {};
    slci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    slci.bindingCount = 4;
    slci.pBindings = bindings;
    if (createSetLayout(m_pCtx->Device(), &slci, nullptr, &m_setLayout) != VK_SUCCESS) {
        Fail("vkCreateDescriptorSetLayout failed");
        return false;
    }
    VkDescriptorPoolSize sizes[3] = {};
    sizes[0] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxLayers };
    sizes[1] = { VK_DESCRIPTOR_TYPE_SAMPLER, kMaxLayers };
    sizes[2] = { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxLayers * 2 };
    VkDescriptorPoolCreateInfo dpci = {};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = kMaxLayers;
    dpci.poolSizeCount = 3;
    dpci.pPoolSizes = sizes;
    if (createPool(m_pCtx->Device(), &dpci, nullptr, &m_descPool) != VK_SUCCESS) {
        Fail("vkCreateDescriptorPool failed");
        return false;
    }
    VkDescriptorSetAllocateInfo dsai = {};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = m_descPool;
    dsai.pSetLayouts = &m_setLayout;
    for (uint32_t i = 0; i < kMaxLayers; i++) {
        dsai.descriptorSetCount = 1;
        VkResult avr = allocSets(m_pCtx->Device(), &dsai, &m_descSets[i]);
        if (avr != VK_SUCCESS) {
            char msg[96];
            _snprintf_s(msg, sizeof(msg), _TRUNCATE, "vkAllocateDescriptorSets failed res=%d", (int)avr);
            Fail(msg);
            return false;
        }
    }
    VkPipelineLayoutCreateInfo plci = {};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &m_setLayout;
    if (createPipeLayout(m_pCtx->Device(), &plci, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        Fail("vkCreatePipelineLayout failed");
        return false;
    }
    // Render pass writes the shared target with a keyed-mutex handshake, so layout
    // discipline stays simple: everything TRANSFER_SRC_OPTIMAL (host copy reads it).
    VkAttachmentDescription ad = {};
    ad.format = VK_FORMAT_B8G8R8A8_UNORM;
    ad.samples = VK_SAMPLE_COUNT_1_BIT;
    ad.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ad.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
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
    if (createRenderPass(m_pCtx->Device(), &rpci, nullptr, &m_renderPass) != VK_SUCCESS) {
        Fail("vkCreateRenderPass failed");
        return false;
    }
    // Native Vulkan compose target. Everything in the Vulkan path (render pass
    // and NV12 compute) works on this image; the software11 native target is only
    // written on request via CopyToRt.
    auto createImage = (PFN_vkCreateImage)m_pCtx->GetDeviceProc("vkCreateImage");
    auto getImgReq = (PFN_vkGetImageMemoryRequirements)m_pCtx->GetDeviceProc("vkGetImageMemoryRequirements");
    auto bindImgMem = (PFN_vkBindImageMemory)m_pCtx->GetDeviceProc("vkBindImageMemory");
    VkImageCreateInfo nci = {};
    nci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    nci.imageType = VK_IMAGE_TYPE_2D;
    nci.format = VK_FORMAT_B8G8R8A8_UNORM;
    nci.extent = { m_width, m_height, 1 };
    nci.mipLevels = 1;
    nci.arrayLayers = 1;
    nci.samples = VK_SAMPLE_COUNT_1_BIT;
    nci.tiling = VK_IMAGE_TILING_OPTIMAL;
    nci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    nci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    nci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!createImage || !getImgReq || !bindImgMem || createImage(m_pCtx->Device(), &nci, nullptr, &m_nativeImage) != VK_SUCCESS) {
        Fail("vkCreateImage(native compose target) failed");
        return false;
    }
    {
        VkMemoryRequirements nreq = {};
        getImgReq(m_pCtx->Device(), m_nativeImage, &nreq);
        auto getMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)m_pCtx->GetInstanceProc("vkGetPhysicalDeviceMemoryProperties");
        uint32_t ntype = 0;
        if (!PickCoherentHostType(nreq, m_pCtx->Physical(), getMemProps, &ntype)) {
            Fail("no memory type for native compose target");
            return false;
        }
            auto allocMem2 = (PFN_vkAllocateMemory)m_pCtx->GetDeviceProc("vkAllocateMemory");
    VkMemoryAllocateInfo nai = {};
        nai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        nai.allocationSize = nreq.size;
        nai.memoryTypeIndex = ntype;
        if (allocMem2(m_pCtx->Device(), &nai, nullptr, &m_nativeMem) != VK_SUCCESS ||
            bindImgMem(m_pCtx->Device(), m_nativeImage, m_nativeMem, 0) != VK_SUCCESS) {
            Fail("native compose target allocation failed");
            return false;
        }
    }
    VkImageViewCreateInfo vici = {};
    vici.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vici.image = m_nativeImage;
    vici.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vici.format = VK_FORMAT_B8G8R8A8_UNORM;
    vici.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (createImgView(m_pCtx->Device(), &vici, nullptr, &m_nativeView) != VK_SUCCESS) {
        Fail("vkCreateImageView(native) failed");
        return false;
    }
    VkFramebufferCreateInfo fbci = {};
    fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbci.renderPass = m_renderPass;
    fbci.attachmentCount = 1;
    fbci.pAttachments = &m_nativeView;
    fbci.width = m_width;
    fbci.height = m_height;
    fbci.layers = 1;
    if (createFramebuffer(m_pCtx->Device(), &fbci, nullptr, &m_framebuffer) != VK_SUCCESS) {
        Fail("vkCreateFramebuffer failed");
        return false;
    }

    // Sampler mirrors the software blit sampler: linear, clamp, no mips.
    VkSamplerCreateInfo sci = {};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 1.0f;
    if (createSampler(m_pCtx->Device(), &sci, nullptr, &m_sampler) != VK_SUCCESS) {
        Fail("vkCreateSampler failed");
        return false;
    }

    #ifdef _WIN32
    // Shaders: HLSL -> DXIL -> SPIR-V once per encoder init.
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* err = nullptr;
    if (FAILED(D3DCompile(kVkBlitVSSource, strlen(kVkBlitVSSource), "VkBlitVS", nullptr, nullptr,
                          "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL2, 0, &vsBlob, &err))) {
        Fail("VS D3DCompile failed");
        if (err) err->Release();
        return false;
    }
    if (FAILED(D3DCompile(kVkSBSPSSource, strlen(kVkSBSPSSource), "VkSBSPS", nullptr, nullptr,
                          "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL2, 0, &psBlob, &err))) {
        vsBlob->Release();
        Fail("PS D3DCompile failed");
        if (err) err->Release();
        return false;
    }
    std::vector<uint32_t> vsWords, psWords;
    bool ok = spirv::TranslateToSpirv(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), "VkBlitVS", vsWords) &&
              spirv::TranslateToSpirv(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), "VkSBSPS", psWords);
    vsBlob->Release();
    psBlob->Release();
    if (!ok) { Fail("SPIR-V translation failed"); return false; }
#else
    // Linux: the build compiles the same HLSL with dxc and embeds the SPIR-V.
    std::vector<uint32_t> vsWords(std::begin(CBPPSpirv_compose_vs), std::end(CBPPSpirv_compose_vs));
    std::vector<uint32_t> psWords(std::begin(CBPPSpirv_compose_ps), std::end(CBPPSpirv_compose_ps));
    // The Windows pipeline retargets dxbc-spirv output to SPIR-V 1.5; dxc targets
    // 1.6 here, so the same downgrade keeps compatibility with Vulkan 1.2 drivers.
    vsWords[1] = 0x00010500;
    psWords[1] = 0x00010500;
#endif
    DriverLog("VK-COMPOSE: spirv words vs=%zu ps=%zu", vsWords.size(), psWords.size());
    VkShaderModule vsMod, psMod;
    VkShaderModuleCreateInfo smci = {};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = vsWords.size() * 4;
    smci.pCode = vsWords.data();
    VkResult vsr = createShaderModule(m_pCtx->Device(), &smci, nullptr, &vsMod);
    if (vsr != VK_SUCCESS) {
        char msg[96];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE, "vkCreateShaderModule(VS) failed res=%d", (int)vsr);
        Fail(msg);
        return false;
    }
    smci.codeSize = psWords.size() * 4;
    smci.pCode = psWords.data();
    VkResult psr = createShaderModule(m_pCtx->Device(), &smci, nullptr, &psMod);
    if (psr != VK_SUCCESS) {
        auto destroyMod0 = (PFN_vkDestroyShaderModule)m_pCtx->GetDeviceProc("vkDestroyShaderModule");
        if (destroyMod0) destroyMod0(m_pCtx->Device(), vsMod, nullptr);
        char msg[96];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE, "vkCreateShaderModule(PS) failed res=%d", (int)psr);
        Fail(msg);
        return false;
    }
    auto destroyMod = (PFN_vkDestroyShaderModule)m_pCtx->GetDeviceProc("vkDestroyShaderModule");

    // Two pipelines, same DAG, differing only in blend state (opaque and alpha).
    // Winding/culling off — the fullscreen triangle must never be clipped by orientation.
    const bool blendFlags[2] = { false, true };
    for (int i = 0; i < 2; i++) {
        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vsMod;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = psMod;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi = {};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo ia = {};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp = {};
        vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        VkViewport viewport = { 0.0f, 0.0f, (float)m_width, (float)m_height, 0.0f, 1.0f };
        VkRect2D scissor = { { 0, 0 }, { m_width, m_height } };
        vp.viewportCount = 1;
        vp.pViewports = &viewport;
        vp.scissorCount = 1;
        vp.pScissors = &scissor;
        VkPipelineRasterizationStateCreateInfo rs = {};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms = {};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds = {};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        VkPipelineColorBlendAttachmentState cba = {};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb = {};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;
        if (blendFlags[i]) {
            cba.blendEnable = VK_TRUE;
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cba.colorBlendOp = VK_BLEND_OP_ADD;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cba.alphaBlendOp = VK_BLEND_OP_ADD;
        }
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
        gpci.pColorBlendState = &cb;
        gpci.layout = m_pipelineLayout;
        gpci.renderPass = m_renderPass;
        gpci.subpass = 0;
        VkPipeline* dst = (i == 0) ? &m_pipelineOpaque : &m_pipelineAlpha;
        if (createPipeline(m_pCtx->Device(), VK_NULL_HANDLE, 1, &gpci, nullptr, dst) != VK_SUCCESS) {
            destroyMod(m_pCtx->Device(), vsMod, nullptr);
            destroyMod(m_pCtx->Device(), psMod, nullptr);
            Fail("vkCreateGraphicsPipelines failed");
            return false;
        }
    }
    destroyMod(m_pCtx->Device(), vsMod, nullptr);
    destroyMod(m_pCtx->Device(), psMod, nullptr);
    m_ready = true;
    DriverLog("VK-COMPOSE initialized: %ux%u pipelines ready", m_width, m_height);
    // The NV12 compute pass is optional: a failure here keeps the compose
    // running and leaves the CPU swscale conversion in place.
    if (!SetupNv12()) {
        DriverLog("VK-NV12 unavailable; staying on CPU swscale");
    }
    return true;
}

// Creates the BGRA->NV12 compute pipeline: the composed RT is read as a sampled
// image and the NV12 planes are written into a host-visible storage buffer.
// Separate from Setup so a failure only costs the fast path.
static bool PickCoherentHostType(const VkMemoryRequirements& req,
                                 VkPhysicalDevice phys,
                                 PFN_vkGetPhysicalDeviceMemoryProperties getMemProps,
                                 uint32_t* outType);

bool VulkanCompose::SetupNv12() {
    auto fail = [](const char* why) { DriverLog("VK-NV12 setup failed: %s", why); return false; };
    m_nv12Size = (size_t)m_width * m_height * 3 / 2;
    auto createShaderModule = (PFN_vkCreateShaderModule)m_pCtx->GetDeviceProc("vkCreateShaderModule");
    auto createSetLayout = (PFN_vkCreateDescriptorSetLayout)m_pCtx->GetDeviceProc("vkCreateDescriptorSetLayout");
    auto createPool = (PFN_vkCreateDescriptorPool)m_pCtx->GetDeviceProc("vkCreateDescriptorPool");
    auto allocSets = (PFN_vkAllocateDescriptorSets)m_pCtx->GetDeviceProc("vkAllocateDescriptorSets");
    auto updateSets = (PFN_vkUpdateDescriptorSets)m_pCtx->GetDeviceProc("vkUpdateDescriptorSets");
    auto createPipeLayout = (PFN_vkCreatePipelineLayout)m_pCtx->GetDeviceProc("vkCreatePipelineLayout");
    auto createPipe = (PFN_vkCreateComputePipelines)m_pCtx->GetDeviceProc("vkCreateComputePipelines");
    auto createBuffer = (PFN_vkCreateBuffer)m_pCtx->GetDeviceProc("vkCreateBuffer");
    auto getBufReq = (PFN_vkGetBufferMemoryRequirements)m_pCtx->GetDeviceProc("vkGetBufferMemoryRequirements");
    auto allocMem = (PFN_vkAllocateMemory)m_pCtx->GetDeviceProc("vkAllocateMemory");
    auto bindBuffer = (PFN_vkBindBufferMemory)m_pCtx->GetDeviceProc("vkBindBufferMemory");
    auto mapMem = (PFN_vkMapMemory)m_pCtx->GetDeviceProc("vkMapMemory");
    auto getMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)m_pCtx->GetInstanceProc("vkGetPhysicalDeviceMemoryProperties");
    if (!createShaderModule || !createSetLayout || !createPool || !allocSets || !updateSets ||
        !createPipeLayout || !createPipe || !createBuffer || !getBufReq || !allocMem ||
        !bindBuffer || !mapMem || !getMemProps) {
        return fail("commands missing");
    }
    #ifdef _WIN32
    ID3DBlob* csBlob = nullptr;
    ID3DBlob* csErr = nullptr;
    if (FAILED(D3DCompile(kNv12CSSource, strlen(kNv12CSSource), "VkNv12CS", nullptr, nullptr,
                          "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL2, 0, &csBlob, &csErr))) {
        if (csErr) csErr->Release();
        return fail("D3DCompile(cs)");
    }
    std::vector<uint32_t> csWords;
    bool csOk = spirv::TranslateToSpirv(csBlob->GetBufferPointer(), csBlob->GetBufferSize(), "VkNv12CS", csWords);
    csBlob->Release();
    if (!csOk) return fail("SPIR-V translation failed");
#else
    std::vector<uint32_t> csWords(std::begin(CBPPSpirv_nv12_cs), std::end(CBPPSpirv_nv12_cs));
    csWords[1] = 0x00010500;
#endif
    VkShaderModuleCreateInfo smci = {};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = csWords.size() * 4;
    smci.pCode = csWords.data();
    if (createShaderModule(m_pCtx->Device(), &smci, nullptr, &m_csMod) != VK_SUCCESS) return fail("vkCreateShaderModule");

    // Host-visible params + NV12 buffers; coherent memory means no per-frame flush.
    auto makeHostBuffer = [&](VkBufferUsageFlags usage, size_t size, VkBuffer* outBuf, VkDeviceMemory* outMem) {
        VkBufferCreateInfo bci = {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = size;
        bci.usage = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (createBuffer(m_pCtx->Device(), &bci, nullptr, outBuf) != VK_SUCCESS) return false;
        VkMemoryRequirements req = {};
        getBufReq(m_pCtx->Device(), *outBuf, &req);
        uint32_t type = 0;
        if (!PickCoherentHostType(req, m_pCtx->Physical(), getMemProps, &type)) return false;
        VkMemoryAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        if (allocMem(m_pCtx->Device(), &ai, nullptr, outMem) != VK_SUCCESS) return false;
        return bindBuffer(m_pCtx->Device(), *outBuf, *outMem, 0) == VK_SUCCESS;
    };
    if (!makeHostBuffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 16, &m_csParams, &m_csParamsMem) ||
        !makeHostBuffer(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m_nv12Size, &m_nv12Buf, &m_nv12Mem)) {
        return false;
    }
    void* paramMap = nullptr;
    if (mapMem(m_pCtx->Device(), m_csParamsMem, 0, 16, 0, &paramMap) != VK_SUCCESS) return fail("map params");
    int32_t dims[4] = { (int32_t)m_width, (int32_t)m_height, (int32_t)(m_width * m_height), 0 };
    memcpy(paramMap, dims, sizeof(dims));
    if (mapMem(m_pCtx->Device(), m_nv12Mem, 0, VK_WHOLE_SIZE, 0, (void**)&m_nv12Map) != VK_SUCCESS) return fail("map nv12");

    VkDescriptorSetLayoutBinding csb[3] = {};
    csb[0].binding = 0;
    csb[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    csb[0].descriptorCount = 1;
    csb[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    csb[1].binding = 1;
    csb[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    csb[1].descriptorCount = 1;
    csb[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    csb[2].binding = 2;
    csb[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    csb[2].descriptorCount = 1;
    csb[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo slci = {};
    slci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    slci.bindingCount = 3;
    slci.pBindings = csb;
    if (createSetLayout(m_pCtx->Device(), &slci, nullptr, &m_csSetLayout) != VK_SUCCESS) return fail("vkCreateDescriptorSetLayout");
    VkDescriptorPoolSize sizes[3] = {};
    sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[0].descriptorCount = 1;
    sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    sizes[1].descriptorCount = 1;
    sizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sizes[2].descriptorCount = 1;
    VkDescriptorPoolCreateInfo dpci = {};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 3;
    dpci.pPoolSizes = sizes;
    if (createPool(m_pCtx->Device(), &dpci, nullptr, &m_csPool) != VK_SUCCESS) return fail("vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dsai = {};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = m_csPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &m_csSetLayout;
    if (allocSets(m_pCtx->Device(), &dsai, &m_csSet) != VK_SUCCESS) return fail("vkAllocateDescriptorSets");

    VkDescriptorBufferInfo paramInfo = {};
    paramInfo.buffer = m_csParams;
    paramInfo.offset = 0;
    paramInfo.range = 16;
    VkDescriptorBufferInfo outInfo = {};
    outInfo.buffer = m_nv12Buf;
    outInfo.offset = 0;
    outInfo.range = m_nv12Size;
    VkDescriptorImageInfo rtInfo = {};
    rtInfo.sampler = VK_NULL_HANDLE;
    rtInfo.imageView = m_nativeView;
    rtInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w3[3] = {};
    w3[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w3[0].dstSet = m_csSet;
    w3[0].dstBinding = 0;
    w3[0].descriptorCount = 1;
    w3[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w3[0].pBufferInfo = &paramInfo;
    w3[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w3[1].dstSet = m_csSet;
    w3[1].dstBinding = 1;
    w3[1].descriptorCount = 1;
    w3[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w3[1].pImageInfo = &rtInfo;
    w3[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w3[2].dstSet = m_csSet;
    w3[2].dstBinding = 2;
    w3[2].descriptorCount = 1;
    w3[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w3[2].pBufferInfo = &outInfo;
    updateSets(m_pCtx->Device(), 3, w3, 0, nullptr);

    VkPipelineLayoutCreateInfo plci = {};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &m_csSetLayout;
    if (createPipeLayout(m_pCtx->Device(), &plci, nullptr, &m_csPipeLayout) != VK_SUCCESS) return fail("vkCreatePipelineLayout");
    VkComputePipelineCreateInfo cpci = {};
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = m_csMod;
    cpci.stage.pName = "main";
    cpci.layout = m_csPipeLayout;
    if (createPipe(m_pCtx->Device(), VK_NULL_HANDLE, 1, &cpci, nullptr, &m_csPipe) != VK_SUCCESS) return fail("vkCreateComputePipelines");
    m_nv12Ready = true;
    DriverLog("VK-NV12 ready: compute path converts %ux%u BGRA -> NV12", m_width, m_height);
    return true;
}

void VulkanCompose::ShutdownNv12() {
    m_nv12Ready = false;
    if (!m_pCtx) return;
    auto* d = m_pCtx->Device();
    auto unmapMem = (PFN_vkUnmapMemory)m_pCtx->GetDeviceProc("vkUnmapMemory");
    auto destroyBuffer = (PFN_vkDestroyBuffer)m_pCtx->GetDeviceProc("vkDestroyBuffer");
    auto freeMem = (PFN_vkFreeMemory)m_pCtx->GetDeviceProc("vkFreeMemory");
    auto destroyPipe = (PFN_vkDestroyPipeline)m_pCtx->GetDeviceProc("vkDestroyPipeline");
    auto destroyShaderModule = (PFN_vkDestroyShaderModule)m_pCtx->GetDeviceProc("vkDestroyShaderModule");
    auto destroyPipeLayout = (PFN_vkDestroyPipelineLayout)m_pCtx->GetDeviceProc("vkDestroyPipelineLayout");
    auto destroySetLayout = (PFN_vkDestroyDescriptorSetLayout)m_pCtx->GetDeviceProc("vkDestroySetLayout");
    auto destroyPool = (PFN_vkDestroyDescriptorPool)m_pCtx->GetDeviceProc("vkDestroyDescriptorPool");
    if (m_nv12Map && unmapMem) { unmapMem(d, m_nv12Mem); m_nv12Map = nullptr; }
    if (m_nv12Buf && destroyBuffer) { destroyBuffer(d, m_nv12Buf, nullptr); m_nv12Buf = VK_NULL_HANDLE; }
    if (m_nv12Mem && freeMem) { freeMem(d, m_nv12Mem, nullptr); m_nv12Mem = VK_NULL_HANDLE; }
    if (m_csParams && destroyBuffer) { destroyBuffer(d, m_csParams, nullptr); m_csParams = VK_NULL_HANDLE; }
    if (m_csParamsMem && freeMem) { freeMem(d, m_csParamsMem, nullptr); m_csParamsMem = VK_NULL_HANDLE; }
    if (m_csPipe && destroyPipe) { destroyPipe(d, m_csPipe, nullptr); m_csPipe = VK_NULL_HANDLE; }
    if (m_csMod && destroyShaderModule) { destroyShaderModule(d, m_csMod, nullptr); m_csMod = VK_NULL_HANDLE; }
    if (m_csPipeLayout && destroyPipeLayout) { destroyPipeLayout(d, m_csPipeLayout, nullptr); m_csPipeLayout = VK_NULL_HANDLE; }
    if (m_csSetLayout && destroySetLayout) { destroySetLayout(d, m_csSetLayout, nullptr); m_csSetLayout = VK_NULL_HANDLE; }
    if (m_csPool && destroyPool) { destroyPool(d, m_csPool, nullptr); m_csPool = VK_NULL_HANDLE; }
    m_csSet = VK_NULL_HANDLE;
    m_nv12Size = 0;
}

// One-time command pool, fence, and bounds buffers (host-visible 32B x kMaxLayers).
// Bounds buffers require coherent host memory (no flush needed per frame).
static bool PickCoherentHostType(const VkMemoryRequirements& req,
                                 VkPhysicalDevice phys,
                                 PFN_vkGetPhysicalDeviceMemoryProperties getMemProps,
                                 uint32_t* outType) {
    VkPhysicalDeviceMemoryProperties memProps = {};
    getMemProps(phys, &memProps);
    for (uint32_t t = 0; t < memProps.memoryTypeCount; t++) {
        if ((req.memoryTypeBits & (1u << t)) == 0) continue;
        uint32_t props = memProps.memoryTypes[t].propertyFlags;
        if ((props & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            *outType = t;
            return true;
        }
    }
    return false;
}

bool VulkanCompose::LazyInit() {
    if (m_lazyDone) return true;
    auto createPool = (PFN_vkCreateCommandPool)m_pCtx->GetDeviceProc("vkCreateCommandPool");
    auto allocCb = (PFN_vkAllocateCommandBuffers)m_pCtx->GetDeviceProc("vkAllocateCommandBuffers");
    auto createFence = (PFN_vkCreateFence)m_pCtx->GetDeviceProc("vkCreateFence");
    auto createBuffer = (PFN_vkCreateBuffer)m_pCtx->GetDeviceProc("vkCreateBuffer");
    auto getBufReq = (PFN_vkGetBufferMemoryRequirements)m_pCtx->GetDeviceProc("vkGetBufferMemoryRequirements");
    auto allocMem = (PFN_vkAllocateMemory)m_pCtx->GetDeviceProc("vkAllocateMemory");
    auto bindBuffer = (PFN_vkBindBufferMemory)m_pCtx->GetDeviceProc("vkBindBufferMemory");
    auto mapMem = (PFN_vkMapMemory)m_pCtx->GetDeviceProc("vkMapMemory");
    auto getMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)m_pCtx->GetInstanceProc("vkGetPhysicalDeviceMemoryProperties");
    if (!createPool || !allocCb || !createFence || !createBuffer || !getBufReq ||
        !allocMem || !bindBuffer || !mapMem || !getMemProps) {
        Fail("lazy commands missing");
        return false;
    }
    VkCommandPoolCreateInfo pci = {};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = m_pCtx->GetQueueFamily();
    if (createPool(m_pCtx->Device(), &pci, nullptr, &m_cmdPool) != VK_SUCCESS) {
        Fail("vkCreateCommandPool(compose) failed");
        return false;
    }
    VkCommandBufferAllocateInfo cbi = {};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = m_cmdPool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    if (allocCb(m_pCtx->Device(), &cbi, &m_cmd) != VK_SUCCESS) {
        Fail("vkAllocateCommandBuffers(compose) failed");
        return false;
    }
    VkFenceCreateInfo fci = {};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (createFence(m_pCtx->Device(), &fci, nullptr, &m_fence) != VK_SUCCESS) {
        Fail("vkCreateFence(compose) failed");
        return false;
    }
    for (uint32_t i = 0; i < kMaxLayers; i++) {
        VkBufferCreateInfo bci = {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = 32;
        bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (createBuffer(m_pCtx->Device(), &bci, nullptr, &m_bounds[i].buf) != VK_SUCCESS) {
            Fail("vkCreateBuffer(bounds) failed");
            return false;
        }
        VkMemoryRequirements req = {};
        getBufReq(m_pCtx->Device(), m_bounds[i].buf, &req);
        int bestScore = -1;
        uint32_t bestType = 0;
        if (PickCoherentHostType(req, m_pCtx->Physical(), getMemProps, &bestType)) bestScore = 0;
        if (bestScore < 0) { Fail("no coherent host-visible bounds type"); return false; }
        VkMemoryAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = bestType;
        if (allocMem(m_pCtx->Device(), &ai, nullptr, &m_bounds[i].mem) != VK_SUCCESS) {
            Fail("vkAllocateMemory(bounds) failed");
            return false;
        }
        if (bindBuffer(m_pCtx->Device(), m_bounds[i].buf, m_bounds[i].mem, 0) != VK_SUCCESS) {
            Fail("vkBindBufferMemory(bounds) failed");
            return false;
        }
        void* map = nullptr;
        if (mapMem(m_pCtx->Device(), m_bounds[i].mem, 0, 32, 0, &map) != VK_SUCCESS) {
            Fail("vkMapMemory(bounds) failed");
            return false;
        }
        m_bounds[i].map = (float*)map;
    }
    m_lazyDone = true;
    return true;
}
// Imports one compositor eye image into Vulkan, cached by handle + geometry so a
// layer-copy recreate (new handle, same size) does not reuse a stale import.
int VulkanCompose::FindOrImportEye(const GpuImage& src) {
    if (!m_pCtx || !m_pCtx->IsReady()) return -1;
    if (!src.handle) {
        Fail("eye image has no handle");
        return -1;
    }
    for (size_t i = 0; i < m_aux.size(); i++) {
        if (m_aux[i].handle == src.handle && m_aux[i].width == src.width &&
            m_aux[i].height == src.height && m_aux[i].srcFormat == src.format)
            return (int)i;
    }
    auto createImage = (PFN_vkCreateImage)m_pCtx->GetDeviceProc("vkCreateImage");
    auto getImgReq = (PFN_vkGetImageMemoryRequirements)m_pCtx->GetDeviceProc("vkGetImageMemoryRequirements");
    auto allocMem = (PFN_vkAllocateMemory)m_pCtx->GetDeviceProc("vkAllocateMemory");
    auto bindImage = (PFN_vkBindImageMemory)m_pCtx->GetDeviceProc("vkBindImageMemory");
    auto createImgView = (PFN_vkCreateImageView)m_pCtx->GetDeviceProc("vkCreateImageView");
    auto getMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)m_pCtx->GetInstanceProc("vkGetPhysicalDeviceMemoryProperties");
    if (!createImage || !getImgReq || !allocMem || !bindImage || !createImgView || !getMemProps) {
        Fail("import commands missing");
        return -1;
    }
    VkImageCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat fmt;
    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    VkMemoryRequirements req = {};
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;

#ifdef _WIN32
    // Windows exports the eye texture as a shared NT handle; Vulkan imports the
    // matching host allocation and binds it to a sampled VkImage.
    fmt = HostFormatToVk((DXGI_FORMAT)src.format);
    if (fmt == VK_FORMAT_UNDEFINED) {
        Fail("unsupported eye texture format");
        return -1;
    }
    ici.format = fmt;
    ici.extent = { src.width, src.height, 1 };
    VkExternalMemoryImageCreateInfo ext = {};
    ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
    ici.pNext = &ext;
    if (createImage(m_pCtx->Device(), &ici, nullptr, &img) != VK_SUCCESS) {
        Fail("vkCreateImage(eye) failed");
        return -1;
    }
    getImgReq(m_pCtx->Device(), img, &req);
    VkImportMemoryWin32HandleInfoKHR import = {};
    import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
    import.handle = (HANDLE)src.handle;
    ai.pNext = &import;
#else
    // Linux exports the eye texture as a dma-buf fd. The DRM format modifier has
    // to match the image, so negotiate the VkFormat against the fd's modifier
    // properties before creating anything.
    fmt = (VkFormat)src.format;
    if (fmt == VK_FORMAT_UNDEFINED) {
        Fail("unsupported eye texture format");
        return -1;
    }
    int fd = (int)src.handle;
    auto getFdProps = (PFN_vkGetMemoryFdPropertiesKHR)m_pCtx->GetDeviceProc("vkGetMemoryFdPropertiesKHR");
    if (!getFdProps || !m_pCtx->HasExternalMemoryFd()) {
        Fail("dma-buf import unavailable");
        return -1;
    }
    VkMemoryFdPropertiesKHR fdProps = {};
    fdProps.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
    if (getFdProps(m_pCtx->Device(), VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fdProps) != VK_SUCCESS) {
        Fail("vkGetMemoryFdPropertiesKHR failed");
        return -1;
    }
    // DRM_FORMAT_MOD_INVALID with any supported format matches the same modifier.
    VkImageDrmFormatModifierExplicitCreateInfoEXT drmInfo = {};
    drmInfo.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
    drmInfo.drmFormatModifier = DRM_FORMAT_MOD_INVALID;
    drmInfo.drmFormatModifierPlaneCount = 1;
    drmInfo.pPlaneLayouts = nullptr;
    ici.pNext = &drmInfo;
    ici.format = fmt;
    ici.extent = { src.width, src.height, 1 };
    VkExternalMemoryImageCreateInfo ext = {};
    ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    ext.pNext = &drmInfo;
    ici.pNext = &ext;
    if (createImage(m_pCtx->Device(), &ici, nullptr, &img) != VK_SUCCESS) {
        Fail("vkCreateImage(eye, dma-buf) failed");
        return -1;
    }
    getImgReq(m_pCtx->Device(), img, &req);
    VkImportMemoryFdInfoKHR fdImport = {};
    fdImport.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    fdImport.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    fdImport.fd = fd;
    ai.pNext = &fdImport;
#endif

    ai.allocationSize = req.size;
    ai.memoryTypeIndex = 0;
    VkPhysicalDeviceMemoryProperties memProps = {};
    getMemProps(m_pCtx->Physical(), &memProps);
    bool found = false;
    for (uint32_t t = 0; t < memProps.memoryTypeCount; t++) {
        if ((req.memoryTypeBits & (1u << t)) != 0) { ai.memoryTypeIndex = t; found = true; break; }
    }
    if (!found) { Fail("no compatible eye memory type"); return -1; }
    if (allocMem(m_pCtx->Device(), &ai, nullptr, &mem) != VK_SUCCESS) {
        Fail("vkAllocateMemory(eye) failed");
        return -1;
    }
    if (bindImage(m_pCtx->Device(), img, mem, 0) != VK_SUCCESS) {
        Fail("vkBindImageMemory(eye) failed");
        return -1;
    }
    VkImageViewCreateInfo ivci = {};
    ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivci.image = img;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = fmt;
    ivci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageView view = VK_NULL_HANDLE;
    if (createImgView(m_pCtx->Device(), &ivci, nullptr, &view) != VK_SUCCESS) {
        Fail("vkCreateImageView(eye) failed");
        return -1;
    }
    AuxImg a;
    a.handle = src.handle;
    a.width = src.width;
    a.height = src.height;
    a.srcFormat = src.format;
    a.img = img;
    a.mem = mem;
    a.view = view;
    a.fmt = fmt;
    a.keyed = src.synchronized;
    m_aux.push_back(a);
    return (int)(m_aux.size() - 1);
}

// Frees imported eye images the last frame did not touch. Their host sources are
// gone (layer copies get recreated per app/session), so holding the imports
// would burn a full-res GPU allocation per stale layer pair.
void VulkanCompose::PruneAux(const int* used, size_t usedCount) {
    for (size_t i = 0; i < m_aux.size();) {
        bool keep = false;
        for (size_t u = 0; u < usedCount; u++) {
            if (used[u] == (int)i) { keep = true; break; }
        }
        if (keep) { i++; continue; }
        AuxImg& a = m_aux[i];
        if (a.view && m_pCtx->GetDeviceProc("vkDestroyImageView")) {
            ((PFN_vkDestroyImageView)m_pCtx->GetDeviceProc("vkDestroyImageView"))(m_pCtx->Device(), a.view, nullptr);
        }
        if (a.img && m_pCtx->GetDeviceProc("vkDestroyImage")) {
            ((PFN_vkDestroyImage)m_pCtx->GetDeviceProc("vkDestroyImage"))(m_pCtx->Device(), a.img, nullptr);
        }
        if (a.mem && m_pCtx->GetDeviceProc("vkFreeMemory")) {
            ((PFN_vkFreeMemory)m_pCtx->GetDeviceProc("vkFreeMemory"))(m_pCtx->Device(), a.mem, nullptr);
        }
        m_aux.erase(m_aux.begin() + (long)i);
    }
}

// Composites every submitted layer into the shared target with one render pass on the
// Vulkan queue. Keyed mutex acquire/release of key 0 brackets target and synced eye
// textures, mirroring the software compose path semantics; fails soft otherwise.
bool VulkanCompose::Compose(const std::vector<GpuImage>& lefts,
                            const std::vector<GpuImage>& rights,
                            const std::vector<LayerRect>& leftBounds,
                            const std::vector<LayerRect>& rightBounds) {
    if (!IsReady()) return false;
    size_t n = lefts.size();
    if (n != rights.size() || n == 0 || n > kMaxLayers) {
        Fail("layer count mismatch (1..4 supported)");
        return false;
    }

    int idxL[kMaxLayers] = {};
    int idxR[kMaxLayers] = {};
    for (size_t i = 0; i < n; i++) {
        idxL[i] = FindOrImportEye(lefts[i]);
        idxR[i] = FindOrImportEye(rights[i]);
        if (idxL[i] < 0 || idxR[i] < 0) return false;
    }

    // Bounds: D3D LayerRect -> float4 (x,y,w,h) per eye; LayerBounds has uMin/vMin/uMax/vMax.
    for (size_t i = 0; i < n; i++) {
        const LayerRect& lb = (i < leftBounds.size()) ? leftBounds[i] : LayerRect{};
        const LayerRect& rb = (i < rightBounds.size()) ? rightBounds[i] : LayerRect{};
        m_bounds[i].map[0] = lb.uMin;
        m_bounds[i].map[1] = lb.vMin;
        m_bounds[i].map[2] = lb.uMax - lb.uMin;
        m_bounds[i].map[3] = lb.vMax - lb.vMin;
        m_bounds[i].map[4] = rb.uMin;
        m_bounds[i].map[5] = rb.vMin;
        m_bounds[i].map[6] = rb.uMax - rb.uMin;
        m_bounds[i].map[7] = rb.vMax - rb.vMin;
    }

    auto beginCmd = (PFN_vkBeginCommandBuffer)m_pCtx->GetDeviceProc("vkBeginCommandBuffer");
    auto endCmd = (PFN_vkEndCommandBuffer)m_pCtx->GetDeviceProc("vkEndCommandBuffer");
    auto barrier = (PFN_vkCmdPipelineBarrier)m_pCtx->GetDeviceProc("vkCmdPipelineBarrier");
    auto updateSets = (PFN_vkUpdateDescriptorSets)m_pCtx->GetDeviceProc("vkUpdateDescriptorSets");
    auto beginRender = (PFN_vkCmdBeginRenderPass)m_pCtx->GetDeviceProc("vkCmdBeginRenderPass");
    auto endRender = (PFN_vkCmdEndRenderPass)m_pCtx->GetDeviceProc("vkCmdEndRenderPass");
    auto bindPipe = (PFN_vkCmdBindPipeline)m_pCtx->GetDeviceProc("vkCmdBindPipeline");
    auto bindSets = (PFN_vkCmdBindDescriptorSets)m_pCtx->GetDeviceProc("vkCmdBindDescriptorSets");
    auto cmdDraw = (PFN_vkCmdDraw)m_pCtx->GetDeviceProc("vkCmdDraw");
    auto submitQ = (PFN_vkQueueSubmit)m_pCtx->GetDeviceProc("vkQueueSubmit");
    auto waitFences = (PFN_vkWaitForFences)m_pCtx->GetDeviceProc("vkWaitForFences");
    auto resetFences = (PFN_vkResetFences)m_pCtx->GetDeviceProc("vkResetFences");
    if (!beginCmd || !endCmd || !barrier || !updateSets || !beginRender || !endRender ||
        !bindPipe || !bindSets || !cmdDraw || !submitQ || !waitFences || !resetFences) {
        Fail("compose commands missing");
        return false;
    }

    // Descriptor writes (host-side; sets are not in flight, submit is fenced).
    for (size_t i = 0; i < n; i++) {
        AuxImg& al = m_aux[idxL[i]];
        AuxImg& ar = m_aux[idxR[i]];
        VkDescriptorImageInfo imgInfo[2] = {};
        // Eyes stay in TRANSFER_SRC_OPTIMAL; sampling from that layout is legal.
        imgInfo[0].imageView = al.view;
        imgInfo[0].imageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        imgInfo[1].imageView = ar.view;
        imgInfo[1].imageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkDescriptorImageInfo sampInfo = {};
        sampInfo.sampler = m_sampler;
        VkDescriptorBufferInfo bufInfo = {};
        bufInfo.buffer = m_bounds[i].buf;
        bufInfo.offset = 0;
        bufInfo.range = 32;
        VkWriteDescriptorSet writes[4] = {};
        for (int w = 0; w < 4; w++) writes[w].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = m_descSets[i];
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &bufInfo;
        writes[1].dstSet = m_descSets[i];
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &sampInfo;
        writes[2].dstSet = m_descSets[i];
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].pImageInfo = &imgInfo[0];
        writes[3].dstSet = m_descSets[i];
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[3].dstBinding = 3;
        writes[3].descriptorCount = 1;
        writes[3].pImageInfo = &imgInfo[1];
        updateSets(m_pCtx->Device(), 4, writes, 0, nullptr);
    }

    // Single command buffer: one-time begin, layout transitions, render pass.
    VkCommandBufferBeginInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (beginCmd(m_cmd, &bi) != VK_SUCCESS) {
        Fail("beginCommandBuffer(compose) failed");
        return false;
    }

    // Eye images: UNDEFINED -> TRANSFER_SRC_OPTIMAL once on first use. After
    // that the layout persists across D3D handshakes (sync object members don't
    // touch layouts) and further submits can draw straight away.
    VkImageMemoryBarrier imb = {};
    imb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    imb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    imb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imb.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    for (size_t i = 0; i < n; i++) {
        AuxImg& al = m_aux[idxL[i]];
        AuxImg& ar = m_aux[idxR[i]];
        if (!al.layoutReady || !ar.layoutReady) {
            if (!al.layoutReady) { imb.image = al.img; barrier(m_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &imb); al.layoutReady = true; }
            if (!ar.layoutReady) { imb.image = ar.img; barrier(m_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &imb); ar.layoutReady = true; }
        }
    }

    // Native target only needs its first UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL
    // transition; the render pass leaves it in COLOR_ATTACHMENT_OPTIMAL.
    if (!m_nativeLayoutReady) {
        VkImageMemoryBarrier first = {};
        first.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        first.srcAccessMask = 0;
        first.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        first.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        first.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        first.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        first.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        first.image = m_nativeImage;
        first.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        first.subresourceRange.levelCount = 1;
        first.subresourceRange.layerCount = 1;
        barrier(m_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                0, 0, nullptr, 0, nullptr, 1, &first);
        m_nativeLayoutReady = true;
    }

    VkRenderPassBeginInfo rpi = {};
    rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpi.renderPass = m_renderPass;
    rpi.framebuffer = m_framebuffer;
    rpi.renderArea = { { 0, 0 }, { m_width, m_height } };
    VkClearValue cv = {};
    rpi.clearValueCount = 1;
    rpi.pClearValues = &cv;
    beginRender(m_cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);
    for (size_t i = 0; i < n; i++) {
        bindPipe(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, (i == 0) ? m_pipelineOpaque : m_pipelineAlpha);
        VkDescriptorSet set = m_descSets[i];
        bindSets(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &set, 0, nullptr);
        cmdDraw(m_cmd, 3, 1, 0, 0);
    }
    endRender(m_cmd);
    if (endCmd(m_cmd) != VK_SUCCESS) {
        Fail("endCommandBuffer(compose) failed");
        return false;
    }

    // Keyed mutex entries: the eye textures are D3D-shared, so their keys must
    // be held for the draw. The native target needs no cross-API sync.
    VkDeviceMemory syncs[2 * kMaxLayers] = {};
    uint64_t syncKeys[2 * kMaxLayers] = {};
    uint32_t syncTimeouts[2 * kMaxLayers] = {};
    uint32_t syncCount = 0;
    for (size_t i = 0; i < n; i++) {
        for (int e = 0; e < 2; e++) {
            AuxImg& a = m_aux[(e == 0) ? idxL[i] : idxR[i]];
            if (a.keyed) {
                syncs[syncCount] = a.mem;
                syncKeys[syncCount] = 0;
                syncTimeouts[syncCount] = 200;
                syncCount++;
            } else {
                // D3D writes these same-device textures; no cross-API handoff
                // is possible here, so a missing key is fine (software compose path
                // also reads them without a mutex).
            }
        }
    }
    if (!m_pCtx->HasKeyedMutex()) { Fail("sync object extension missing"); return false; }
    VkWin32KeyedMutexAcquireReleaseInfoKHR km = {};
    km.sType = VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR;
    km.acquireCount = syncCount;
    km.pAcquireSyncs = syncs;
    km.pAcquireKeys = syncKeys;
    km.pAcquireTimeouts = syncTimeouts;
    km.releaseCount = syncCount;
    km.pReleaseSyncs = syncs;
    km.pReleaseKeys = syncKeys;
    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.pNext = &km;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &m_cmd;
    resetFences(m_pCtx->Device(), 1, &m_fence);
    VkResult res = submitQ(m_pCtx->Queue(), 1, &si, m_fence);
    if (res != VK_SUCCESS) {
        // Transient (a busy queue right after a SteamVR restart is the known
        // case): keep the pipeline and let the caller fall back for this frame.
        m_submitFails++;
        if (m_submitFails <= 3 || m_submitFails % 300 == 0) {
            DriverLog("VK-COMPOSE: vkQueueSubmit failed res=%d (fail=%d), using D3D this frame",
                      (int)res, m_submitFails);
        }
        return false;
    }
    m_submitFails = 0;
    res = waitFences(m_pCtx->Device(), 1, &m_fence, VK_TRUE, 1000 * 1000000ULL);
    if (res != VK_SUCCESS) {
        Fail("fence wait(compose) failed");
        return false;
    }
    m_frame++;
    int used[2 * kMaxLayers] = {};
    for (size_t i = 0; i < n; i++) {
        used[2 * i] = idxL[i];
        used[2 * i + 1] = idxR[i];
    }
    PruneAux(used, 2 * n);
    if (m_frame <= 2 || m_frame % 300 == 0) {
        DriverLog("VK-COMPOSE: frame=%llu layers=%zu", (unsigned long long)m_frame, n);
    }
    return true;
}


// Runs the NV12 compute pass over the composed RT and hands back the mapped
// host buffer. The RT stays in TRANSFER_SRC_OPTIMAL (shader reads are legal
// there), so only a same-layout write->read barrier is needed.
// Runs the NV12 compute pass over the natively composed frame and hands back
// the mapped host buffer. The native image goes COLOR_ATTACHMENT_OPTIMAL ->
// SHADER_READ_ONLY_OPTIMAL -> COLOR_ATTACHMENT_OPTIMAL so the next compose pass
// finds the layout its render pass declares.
bool VulkanCompose::ToNv12(const uint8_t** outData, size_t* outSize) {
    if (!m_nv12Ready || !m_pCtx || !m_pCtx->IsReady()) return false;
    auto beginCmd = (PFN_vkBeginCommandBuffer)m_pCtx->GetDeviceProc("vkBeginCommandBuffer");
    auto endCmd = (PFN_vkEndCommandBuffer)m_pCtx->GetDeviceProc("vkEndCommandBuffer");
    auto barrier = (PFN_vkCmdPipelineBarrier)m_pCtx->GetDeviceProc("vkCmdPipelineBarrier");
    auto bindPipe = (PFN_vkCmdBindPipeline)m_pCtx->GetDeviceProc("vkCmdBindPipeline");
    auto bindSets = (PFN_vkCmdBindDescriptorSets)m_pCtx->GetDeviceProc("vkCmdBindDescriptorSets");
    auto dispatch = (PFN_vkCmdDispatch)m_pCtx->GetDeviceProc("vkCmdDispatch");
    auto submitQ = (PFN_vkQueueSubmit)m_pCtx->GetDeviceProc("vkQueueSubmit");
    auto waitFences = (PFN_vkWaitForFences)m_pCtx->GetDeviceProc("vkWaitForFences");
    auto resetFences = (PFN_vkResetFences)m_pCtx->GetDeviceProc("vkResetFences");
    if (!beginCmd || !endCmd || !barrier || !bindPipe || !bindSets || !dispatch ||
        !submitQ || !waitFences || !resetFences) {
        return false;
    }
    VkCommandBufferBeginInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (beginCmd(m_cmd, &bi) != VK_SUCCESS) return false;
    VkImageMemoryBarrier imb = {};
    imb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    imb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    imb.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    imb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imb.image = m_nativeImage;
    imb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imb.subresourceRange.levelCount = 1;
    imb.subresourceRange.layerCount = 1;
    barrier(m_cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &imb);
    bindPipe(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_csPipe);
    bindSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_csPipeLayout, 0, 1, &m_csSet, 0, nullptr);
    dispatch(m_cmd, (m_width / 8 + 7) / 8, (m_height + 7) / 8, 1);
    VkImageMemoryBarrier back = imb;
    back.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier(m_cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, 0, nullptr, 0, nullptr, 1, &back);
    if (endCmd(m_cmd) != VK_SUCCESS) return false;
    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &m_cmd;
    resetFences(m_pCtx->Device(), 1, &m_fence);
    if (submitQ(m_pCtx->Queue(), 1, &si, m_fence) != VK_SUCCESS) {
        m_submitFails++;
        if (m_submitFails <= 3 || m_submitFails % 300 == 0) {
            DriverLog("VK-NV12: vkQueueSubmit failed (fail=%d), using CPU swscale", m_submitFails);
        }
        return false;
    }
    if (waitFences(m_pCtx->Device(), 1, &m_fence, VK_TRUE, 1000 * 1000000ULL) != VK_SUCCESS) {
        return false;
    }
    *outData = m_nv12Map;
    *outSize = m_nv12Size;
    return true;
}


// Copies the composed frame into a host-visible buffer. Two users: the CPU
// swscale fallback when the NV12 compute pass did not run, and the frame hash
// diagnostic (same FNV-1a sampling as the software duplicate-frame check).
void VulkanCompose::CopyToHost()
{
    if (!m_ready || !m_pCtx || !m_pCtx->IsReady()) return;
    if (!m_hostMap) {
        auto createBuffer = (PFN_vkCreateBuffer)m_pCtx->GetDeviceProc("vkCreateBuffer");
        auto getBufReq = (PFN_vkGetBufferMemoryRequirements)m_pCtx->GetDeviceProc("vkGetBufferMemoryRequirements");
        auto allocMem = (PFN_vkAllocateMemory)m_pCtx->GetDeviceProc("vkAllocateMemory");
        auto bindBuffer = (PFN_vkBindBufferMemory)m_pCtx->GetDeviceProc("vkBindBufferMemory");
        auto mapMem = (PFN_vkMapMemory)m_pCtx->GetDeviceProc("vkMapMemory");
        auto getMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)m_pCtx->GetInstanceProc("vkGetPhysicalDeviceMemoryProperties");
        if (!createBuffer || !getBufReq || !allocMem || !bindBuffer || !mapMem || !getMemProps) return;
        m_hostSize = (size_t)m_width * m_height * 4;
        VkBufferCreateInfo bci = {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = m_hostSize;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (createBuffer(m_pCtx->Device(), &bci, nullptr, &m_hostBuf) != VK_SUCCESS) return;
        VkMemoryRequirements req = {};
        getBufReq(m_pCtx->Device(), m_hostBuf, &req);
        uint32_t type = 0;
        if (!PickCoherentHostType(req, m_pCtx->Physical(), getMemProps, &type)) return;
        VkMemoryAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        if (allocMem(m_pCtx->Device(), &ai, nullptr, &m_hostMem) != VK_SUCCESS) return;
        if (bindBuffer(m_pCtx->Device(), m_hostBuf, m_hostMem, 0) != VK_SUCCESS) return;
        if (mapMem(m_pCtx->Device(), m_hostMem, 0, VK_WHOLE_SIZE, 0, (void**)&m_hostMap) != VK_SUCCESS) {
            m_hostMap = nullptr;
            return;
        }
        DriverLog("VK-COMPOSE: host copy buffer mapped (%zu bytes)", m_hostSize);
    }
    auto beginCmd = (PFN_vkBeginCommandBuffer)m_pCtx->GetDeviceProc("vkBeginCommandBuffer");
    auto endCmd = (PFN_vkEndCommandBuffer)m_pCtx->GetDeviceProc("vkEndCommandBuffer");
    auto barrier = (PFN_vkCmdPipelineBarrier)m_pCtx->GetDeviceProc("vkCmdPipelineBarrier");
    auto copyImage = (PFN_vkCmdCopyImageToBuffer)m_pCtx->GetDeviceProc("vkCmdCopyImageToBuffer");
    auto submitQ = (PFN_vkQueueSubmit)m_pCtx->GetDeviceProc("vkQueueSubmit");
    auto waitFences = (PFN_vkWaitForFences)m_pCtx->GetDeviceProc("vkWaitForFences");
    auto resetFences = (PFN_vkResetFences)m_pCtx->GetDeviceProc("vkResetFences");
    if (!beginCmd || !endCmd || !barrier || !copyImage || !submitQ || !waitFences || !resetFences) return;
    VkCommandBufferBeginInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (beginCmd(m_cmd, &bi) != VK_SUCCESS) return;
    VkImageMemoryBarrier imb = {};
    imb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    imb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    imb.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    imb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    imb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imb.image = m_nativeImage;
    imb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imb.subresourceRange.levelCount = 1;
    imb.subresourceRange.layerCount = 1;
    barrier(m_cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &imb);
    VkBufferImageCopy region = {};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { m_width, m_height, 1 };
    copyImage(m_cmd, m_nativeImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_hostBuf, 1, &region);
    // Hand the image back for the next compose pass.
    VkImageMemoryBarrier back = imb;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier(m_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, 0, nullptr, 0, nullptr, 1, &back);
    if (endCmd(m_cmd) != VK_SUCCESS) return;
    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &m_cmd;
    resetFences(m_pCtx->Device(), 1, &m_fence);
    if (submitQ(m_pCtx->Queue(), 1, &si, m_fence) != VK_SUCCESS) return;
    if (waitFences(m_pCtx->Device(), 1, &m_fence, VK_TRUE, 1000 * 1000000ULL) != VK_SUCCESS) return;
    m_hostFrame++;
    uint32_t hash = 2166136261u;
    uint64_t sum = 0;
    int samples = 0;
    for (size_t i = 0; i < m_hostSize; i += 1024) {
        hash ^= m_hostMap[i];
        hash *= 16777619u;
        sum += m_hostMap[i];
        samples++;
    }
    if (hash != m_hostHash) {
        m_hostHash = hash;
        m_hostChanges++;
    }
    if (m_hostFrame <= 2 || m_hostFrame % 300 == 0) {
        DriverLog("VK-HOSTCOPY: frame=%llu hash=0x%08x changes=%llu mean=%d",
                  (unsigned long long)m_hostFrame, hash, (unsigned long long)m_hostChanges,
                  (int)(sum / (samples > 0 ? samples : 1)));
    }
}
