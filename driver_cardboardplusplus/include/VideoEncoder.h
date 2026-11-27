#pragma once
#define __STDC_CONSTANT_MACROS
#include <atomic>
#include <vector>
#include <utility>
#include <functional>
#include <mutex>
#include "BridgeProtocol.h"
#include "GpuImage.h"
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVBSFContext;
struct SwsContext;
struct AVBufferRef;
class VulkanContext;
class VulkanContext;
class VulkanCompose;
// SBS H.264 encoder: composites the submitted eye layers in Vulkan, converts the
// composed frame to NV12 and encodes it with FFmpeg.
class VideoEncoder
{
public:
// Packet/telemetry callbacks delivered to HmdDriver; LayerBounds carries per-eye UV rects from SubmitLayer.
    using EncodedPacketCallback = std::function<void(uint8_t* data, int size, int64_t pts, bool keyframe)>;
    using TelemetryCallback = std::function<void(const cbpp::PayloadTelemetry&)>;
    struct LayerBounds {
        float uMin = 0.0f, vMin = 0.0f, uMax = 1.0f, vMax = 1.0f;
    };
// Main pipeline stages driven by the encoding thread: init, compose, read back, convert, encode.
    VideoEncoder();
    ~VideoEncoder();
    bool Initialize(int width, int height, int fps, int bitrate, bool useGpuEncoding,
                    VulkanContext* pVulkan = nullptr);
    void Shutdown();
    bool ComposeSBSGPU(const std::vector<GpuImage>& lefts,
                       const std::vector<GpuImage>& rights,
                       const std::vector<LayerBounds>& leftBounds,
                       const std::vector<LayerBounds>& rightBounds);
    bool ConvertFrame();
    bool SwsConvert();
    bool FinishEncode(int64_t pts);
    bool ReadbackToBuffer();
    void SetEncodedPacketCallback(EncodedPacketCallback callback);
    void SetTelemetryCallback(TelemetryCallback callback);
    void RequestKeyframe();
// Trivial state queries; read by Present/heartbeat paths to check encoder readiness and geometry.
// Internal FFmpeg/shader/compose stages plus error and version logging helpers.
    bool IsInitialized() const { return m_initialized; }
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }
private:
    bool InitializeFFmpeg();
    void CleanupFFmpeg();
    bool InitializeCompose();
    void ShutdownCompose();
    bool SendFrameToEncoder();
    bool ReceiveEncodedPackets();
    void LogFFmpegError(const char* context, int errorCode);
    void LogFFmpegVersion();
// FFmpeg state, the Vulkan composer, and encode timing.
    AVCodecContext* m_pCodecContext;
    AVBufferRef* m_pHwDeviceCtx;
    AVFrame* m_pFrame;
    AVPacket* m_pPacket;
    AVBSFContext* m_pBsfCtx;
    SwsContext* m_pConvertContext;
    bool m_initialized;
    bool m_useGpuEncoding;
// Vulkan composer state; the encoder has no graphics device of its own.
    VulkanContext* m_pVulkan = nullptr;
    VulkanCompose* m_pVkCompose = nullptr;
    int m_width;
    int m_height;
    int m_fps;
    int m_bitrate;
    int64_t m_frameCount;
    EncodedPacketCallback m_encodedPacketCallback;
    TelemetryCallback m_telemetryCallback;
    uint8_t* m_pSoftwareFrameBuffer;
    bool m_hasValidFrame;
    bool m_composeReady;
    uint64_t m_perfFreq = 0;
    int64_t m_encSumUs;
    int64_t m_encMaxUs;
    int64_t m_encCount;
    int64_t m_lastCallUs;
    int64_t m_intervalSumUs;
    int64_t m_intervalMaxUs;
    int64_t m_intervalCount;
    uint32_t m_lastFrameHash;
    int m_dupCount;
    int64_t m_summaryFrames;
    int m_summaryInterval;
    uint32_t ComputeFrameHash();
    void LogTelemetrySummary();
    std::vector<uint8_t> m_spsPpsAnnexB;
    std::atomic<bool> m_forceKeyframe{false};
    uint32_t m_lastLeftFmt = 0;
    uint32_t m_lastRightFmt = 0;
    // Frame source for SwsConvert: the Vulkan host copy when the compute pass did
    // not produce NV12 for this frame (may stay null).
    uint8_t* m_pSwsSrc = nullptr;
    // NV12 computed on the GPU for the current frame (owned by the composer).
    const uint8_t* m_pGpuNv12 = nullptr;
    size_t m_gpuNv12Size = 0;
    uint64_t m_gpuNv12Frames = 0;
    // True when the Vulkan composer owns the frame that the NV12 pass reads.
    bool m_composedOnVulkan = false;
};
