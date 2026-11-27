#include "VideoEncoder.h"
#include "VideoEncoderFFmpeg.h"
#include "VideoEncoderLog.h"
#include "VulkanCompose.h"
// Zero-initializes the FFmpeg handles and telemetry counters.
VideoEncoder::VideoEncoder()
    : m_pCodecContext(nullptr)
    , m_pFrame(nullptr)
    , m_pPacket(nullptr)
    , m_pBsfCtx(nullptr)
    , m_pConvertContext(nullptr)
    , m_initialized(false)
    , m_useGpuEncoding(false)
    , m_width(0)
    , m_height(0)
    , m_fps(0)
    , m_bitrate(0)
    , m_frameCount(0)
    , m_pSoftwareFrameBuffer(nullptr)
    , m_pHwDeviceCtx(nullptr)
    , m_hasValidFrame(false)
    , m_composeReady(false)
    , m_encSumUs(0)
    , m_encMaxUs(0)
    , m_encCount(0)
    , m_lastCallUs(0)
    , m_intervalSumUs(0)
    , m_intervalMaxUs(0)
    , m_intervalCount(0)
    , m_lastFrameHash(0)
    , m_dupCount(0)
    , m_summaryFrames(0)
    , m_summaryInterval(120)
{
}
// Releases FFmpeg and shader resources; called from Shutdown paths and the destructor.
VideoEncoder::~VideoEncoder()
{
    Shutdown();
}
// Builds the FFmpeg codec, the Vulkan compose/conversion pipeline and the software frame buffers for width x height at fps/bitrate.
bool VideoEncoder::Initialize(int width, int height, int fps, int bitrate, bool useGpuEncoding,
                             VulkanContext* pVulkan)
{
    m_pVulkan = pVulkan;
    ENCODER_LOG("========================================");
    ENCODER_LOG("Initializing VideoEncoder");
    ENCODER_LOG("  Resolution: %dx%d", width, height);
    ENCODER_LOG("  FPS: %d", fps);
    ENCODER_LOG("  Bitrate: %d kbps", bitrate / 1000);
    ENCODER_LOG("  GPU Encoding: %s", useGpuEncoding ? "YES" : "NO");
    ENCODER_LOG("========================================");
    if (m_initialized) {
        ENCODER_ERROR("Encoder already initialized!");
        return false;
    }
    if (width <= 0 || height <= 0) {
        ENCODER_ERROR("Invalid dimensions: %dx%d", width, height);
        return false;
    }
    m_width = width;
    m_height = height;
    m_fps = fps;
    m_bitrate = bitrate;
    m_useGpuEncoding = useGpuEncoding;
    m_frameCount = 0;
    // Vulkan owns the whole video path; the compositor device is only used
    // to open the eye textures the composer imports.
    m_pVulkan = pVulkan;
    LogFFmpegVersion();
    if (!InitializeFFmpeg()) {
        ENCODER_ERROR("Failed to initialize FFmpeg!");
        CleanupFFmpeg();
        return false;
    }
    if (!InitializeCompose()) {
        ENCODER_ERROR("Failed to initialize shader conversion!");
        ShutdownCompose();
        CleanupFFmpeg();
        return false;
    }
    m_initialized = true;
    ENCODER_LOG("VideoEncoder initialized successfully!");
    return true;
}
// Frees codec, shader, staging, and eye-texture state and marks the encoder idle; safe to call twice.
void VideoEncoder::Shutdown()
{
    if (!m_initialized && !m_pCodecContext && !m_pFrame && !m_pPacket) {
        return;
    }
    ENCODER_LOG("Shutting down VideoEncoder...");
    if (m_pVkCompose) {
        m_pVkCompose->Shutdown();
        delete m_pVkCompose;
        m_pVkCompose = nullptr;
    }
ShutdownCompose();
    CleanupFFmpeg();
    if (m_pSoftwareFrameBuffer) {
        av_free(m_pSoftwareFrameBuffer);
        m_pSoftwareFrameBuffer = nullptr;
    }
    m_initialized = false;
    ENCODER_LOG("VideoEncoder shutdown complete.");
}
// Registers the HmdDriver packet sink invoked by ReceiveEncodedPackets for each access unit.
void VideoEncoder::SetEncodedPacketCallback(EncodedPacketCallback callback)
{
    m_encodedPacketCallback = callback;
}
// Registers the bridge telemetry sink invoked by LogTelemetrySummary every summaryInterval frames.
void VideoEncoder::SetTelemetryCallback(TelemetryCallback callback)
{
    m_telemetryCallback = callback;
}
