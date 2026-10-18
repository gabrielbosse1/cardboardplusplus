#include "VideoEncoder.h"
#include "VideoEncoderFFmpeg.h"
#include "VideoEncoderLog.h"
#include "VulkanCompose.h"
#define DRIVER_DIAG
#include <cassert>
#include <cstring>

// Brings up the Vulkan composer: it owns the SBS compose render pass, the
// NV12 compute conversion and the host-visible host copy copy. No D3D11 objects
// are created here � the frame never leaves Vulkan.
bool VideoEncoder::InitializeCompose()
{
    ENCODER_LOG("Initializing Vulkan compose + conversion...");
    if (!m_pVulkan || !m_pVulkan->IsReady()) {
        ENCODER_ERROR("No Vulkan device; the encoder cannot run");
        return false;
    }
    m_pVkCompose = new VulkanCompose();
    m_pVkCompose->Init(m_pVulkan);
    if (!m_pVkCompose->Setup((uint32_t)m_width, (uint32_t)m_height)) {
        delete m_pVkCompose;
        m_pVkCompose = nullptr;
        return false;
    }
    m_composeReady = true;
    ENCODER_LOG("Vulkan conversion pipeline ready: %dx%d", m_width, m_height);
    return true;
}

// Releases the Vulkan composer.
void VideoEncoder::ShutdownCompose()
{
    ENCODER_LOG("Cleaning up Vulkan conversion pipeline...");
    if (m_pVkCompose) {
        m_pVkCompose->Shutdown();
        delete m_pVkCompose;
        m_pVkCompose = nullptr;
    }
    m_composeReady = false;
}

// Produces the frame source for the encode step. The Vulkan NV12 compute pass
// normally supplies the encoder frame directly; this runs the GPU conversion
// and, when that did not produce a frame, hands back the host copy's host-visible
// BGRA copy for the CPU swscale fallback.
bool VideoEncoder::ReadbackToBuffer()
{
    if (!m_initialized) {
        ENCODER_ERROR("ReadbackToBuffer called without valid encoder!");
        return false;
    }
    if (!m_pVkCompose) {
        ENCODER_ERROR("ReadbackToBuffer without Vulkan compose");
        return false;
    }
    m_pGpuNv12 = nullptr;
    m_pSwsSrc = nullptr;
    if (m_composedOnVulkan && m_pVkCompose) {
        const uint8_t* nv12 = nullptr;
        size_t nv12Size = 0;
        if (m_pVkCompose->ToNv12(&nv12, &nv12Size)) {
            m_pGpuNv12 = nv12;
            m_gpuNv12Size = nv12Size;
            m_gpuNv12Frames++;
            return true;
        }
    }
    m_pVkCompose->CopyToHost();
    if (m_pVkCompose->HostData()) {
        m_pSwsSrc = (uint8_t*)m_pVkCompose->HostData();
        return true;
    }
    static unsigned s_readbackFail = 0;
    if (++s_readbackFail <= 3 || s_readbackFail % 300 == 0) {
        ENCODER_ERROR("host copy readback unavailable (drop=%u)", s_readbackFail);
    }
    return false;
}

// Hands the submitted eye layers to the Vulkan composer, which draws them into
// its own native target.
bool VideoEncoder::ComposeSBSGPU(const std::vector<GpuImage>& lefts,
                                 const std::vector<GpuImage>& rights,
                                 const std::vector<LayerBounds>& leftBounds,
                                 const std::vector<LayerBounds>& rightBounds)
{
    if (!m_initialized) {
        ENCODER_ERROR("ComposeSBSGPU called without valid encoder!");
        return false;
    }
    if (lefts.empty() || rights.empty() || lefts.size() != rights.size()) {
        ENCODER_ERROR("ComposeSBSGPU called with invalid/mismatched layer counts!");
        return false;
    }
    m_lastLeftFmt = lefts.front().format;
    m_lastRightFmt = rights.front().format;
#ifdef DRIVER_DIAG
    {
        static int s_composeDiag = 0;
        if (++s_composeDiag % 120 == 0) {
            ENCODER_LOG("[DIAG compose] layers=%zu L fmt=0x%x (%ux%u) R fmt=0x%x (%ux%u)",
                lefts.size(),
                lefts.front().format, lefts.front().width, lefts.front().height,
                rights.front().format, rights.front().width, rights.front().height);
        }
    }
#endif
    // Vulkan owns the compose: it draws the same layers into its native target
    // and the NV12 compute pass converts that to the encoder's frame format.
    // No software fallback exists; a Vulkan failure drops the frame.
    if (!m_pVkCompose || !m_pVkCompose->IsReady()) {
        static unsigned s_noCompose = 0;
        if (++s_noCompose <= 3 || s_noCompose % 300 == 0) {
            ENCODER_ERROR("Vulkan compose unavailable (drop=%u)", s_noCompose);
        }
        m_composedOnVulkan = false;
        return false;
    }
    std::vector<LayerRect> lbL(lefts.size()), lbR(rights.size());
    for (size_t i = 0; i < lefts.size(); i++) {
        const LayerBounds& l = (i < leftBounds.size()) ? leftBounds[i] : LayerBounds{};
        const LayerBounds& r = (i < rightBounds.size()) ? rightBounds[i] : LayerBounds{};
        lbL[i] = { l.uMin, l.vMin, l.uMax, l.vMax };
        lbR[i] = { r.uMin, r.vMin, r.uMax, r.vMax };
    }
    if (m_pVkCompose->Compose(lefts, rights, lbL, lbR)) {
        m_composedOnVulkan = true;
        return true;
    }
    m_composedOnVulkan = false;
    static unsigned s_composeFail = 0;
    if (++s_composeFail <= 3 || s_composeFail % 300 == 0) {
        ENCODER_ERROR("Vulkan compose failed (drop=%u)", s_composeFail);
    }
    return false;
}
