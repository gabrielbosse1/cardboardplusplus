#include "VideoEncoder.h"
#include "VideoEncoderFFmpeg.h"
#include "VideoEncoderLog.h"
#include "VulkanCompose.h"
#define DRIVER_DIAG
#include <cassert>
#include <cstdio>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

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

// Frame-dump diagnostics, gated by CARDBOARD_DUMP_FRAMES=<count> in vrserver's
// environment. Writes the first <count> frames to <temp>/cbpp-dumps: the
// composed BGRA side-by-side image (viewable BMP) and the pre-encode NV12
// buffer (raw). Comparing these against the decoded stream bisects whether the
// corruption is introduced before encoding (compose/import) or in the encode
// path. Files are overwritten on every run; the log prints the exact paths.
namespace {
struct FrameDump {
    bool ready = false;
    int remaining = 0;
    char dir[512] = {};
};
FrameDump& DumpState() {
    static FrameDump s;
    return s;
}
void InitDumpState() {
    FrameDump& s = DumpState();
    if (s.ready) return;
    const char* v = cbpp::PlatformGetEnv("CARDBOARD_DUMP_FRAMES");
    if (!v || v[0] < '1' || v[0] > '9') {
        s.ready = true;
        s.remaining = 0;
        return;
    }
    s.remaining = v[0] - '0';
#ifdef _WIN32
    char tmp[MAX_PATH];
    if (GetTempPathA(sizeof(tmp), tmp) == 0) tmp[0] = '\0';
    snprintf(s.dir, sizeof(s.dir), "%scbpp-dumps", tmp);
    CreateDirectoryA(s.dir, nullptr);
#else
    snprintf(s.dir, sizeof(s.dir), "/tmp/cbpp-dumps");
    mkdir(s.dir, 0755);
#endif
    s.ready = true;
    DriverLogFFmpeg("Frame dump enabled: first %d frames -> %s", s.remaining, s.dir);
}
void DumpFile(const char* path, const void* data, size_t size) {
    FILE* f = fopen(path, "wb");
    if (!f) {
        DriverLogFFmpeg("frame dump: fopen %s failed", path);
        return;
    }
    fwrite(data, 1, size, f);
    fclose(f);
}
// 32bpp BI_RGB top-down BMP of a BGRA buffer; opens in any image viewer.
void DumpBgraBmp(const FrameDump& s, unsigned frame, const uint8_t* bgra,
                 int w, int h) {
    const int kHeaderSize = 14 + 40;
    const size_t pixelBytes = (size_t)w * h * 4;
    uint8_t header[kHeaderSize] = {};
    header[0] = 'B'; header[1] = 'M';
    uint32_t fileSize = (uint32_t)(kHeaderSize + pixelBytes);
    memcpy(header + 2, &fileSize, 4);
    header[10] = (uint8_t)(kHeaderSize);
    uint32_t infoSize = 40;
    memcpy(header + 14, &infoSize, 4);
    int32_t bw = w, bh = -h; // negative height = top-down rows
    memcpy(header + 18, &bw, 4);
    memcpy(header + 22, &bh, 4);
    header[26] = 1; // planes
    header[28] = 32; // bpp
    uint32_t imageBytes = (uint32_t)pixelBytes;
    memcpy(header + 34, &imageBytes, 4);
    char path[600];
    snprintf(path, sizeof(path), "%s\\frame%u_%dx%d.bgra.bmp", s.dir, frame, w, h);
    FILE* f = fopen(path, "wb");
    if (!f) {
        DriverLogFFmpeg("frame dump: fopen %s failed", path);
        return;
    }
    fwrite(header, 1, sizeof(header), f);
    fwrite(bgra, 1, pixelBytes, f);
    fclose(f);
    DriverLogFFmpeg("frame dump: wrote %s", path);
}
void DumpNv12(const FrameDump& s, unsigned frame, const uint8_t* nv12,
              int w, int h) {
    char path[600];
#ifdef _WIN32
    snprintf(path, sizeof(path), "%s\\frame%u_%dx%d.nv12", s.dir, frame, w, h);
#else
    snprintf(path, sizeof(path), "%s/frame%u_%dx%d.nv12", s.dir, frame, w, h);
#endif
    DumpFile(path, nv12, (size_t)w * h * 3 / 2);
    DriverLogFFmpeg("frame dump: wrote %s", path);
}
}  // namespace

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
    InitDumpState();
    m_pGpuNv12 = nullptr;
    m_pSwsSrc = nullptr;
    bool dumpThisFrame = DumpState().remaining > 0;
    if (m_composedOnVulkan && m_pVkCompose) {
        const uint8_t* nv12 = nullptr;
        size_t nv12Size = 0;
        if (m_pVkCompose->ToNv12(&nv12, &nv12Size)) {
            m_pGpuNv12 = nv12;
            m_gpuNv12Size = nv12Size;
            m_gpuNv12Frames++;
            if (dumpThisFrame) {
                unsigned frame = (unsigned)DumpState().remaining;
                DumpNv12(DumpState(), frame, nv12, m_width, m_height);
                // The BGRA host copy shares the same composed image; grab it
                // for the same frame so both stages can be compared.
                m_pVkCompose->CopyToHost();
                if (m_pVkCompose->HostData()) {
                    DumpBgraBmp(DumpState(), frame, m_pVkCompose->HostData(), m_width, m_height);
                } else {
                    DriverLogFFmpeg("frame dump: BGRA host copy unavailable this frame");
                }
                DumpState().remaining--;
            }
            return true;
        }
    }
    m_pVkCompose->CopyToHost();
    if (m_pVkCompose->HostData()) {
        m_pSwsSrc = (uint8_t*)m_pVkCompose->HostData();
        if (dumpThisFrame) {
            unsigned frame = (unsigned)DumpState().remaining;
            DriverLogFFmpeg("frame dump: NV12 compute unavailable this frame (CPU fallback)");
            DumpBgraBmp(DumpState(), frame, m_pSwsSrc, m_width, m_height);
            DumpState().remaining--;
        }
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
    static bool loggedComposeEntry = false;
    if (!loggedComposeEntry) {
        loggedComposeEntry = true;
        DriverLogFFmpeg("ComposeSBSGPU entry: layers=%zu Lhandle=%llu Rhandle=%llu",
                        lefts.size(), (unsigned long long)lefts.front().handle,
                        (unsigned long long)rights.front().handle);
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
    static bool loggedComposeCall = false;
    if (!loggedComposeCall) {
        loggedComposeCall = true;
        DriverLogFFmpeg("Calling VulkanCompose::Compose");
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
