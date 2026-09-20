#pragma once
#define __STDC_CONSTANT_MACROS
#include <windows.h>
#include <d3d11.h>
#include <atomic>
#include <vector>
#include <utility>
#include <functional>
#include <mutex>
#include "BridgeProtocol.h"
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVBSFContext;
struct SwsContext;
struct AVBufferRef;
// SBS H.264 encoder: composites eye textures with shaders on D3D11, converts to NV12, encodes via FFmpeg.
class VideoEncoder
{
public:
// Packet/telemetry callbacks delivered to HmdDriver; LayerBounds carries per-eye UV rects from SubmitLayer.
    using EncodedPacketCallback = std::function<void(uint8_t* data, int size, int64_t pts, bool keyframe)>;
    using TelemetryCallback = std::function<void(const cbpp::PayloadTelemetry&)>;
    struct LayerBounds {
        float uMin = 0.0f, vMin = 0.0f, uMax = 1.0f, vMax = 1.0f;
    };
// Main pipeline stages driven by the encoding thread: init, open eyes, compose, read back, convert, encode.
    VideoEncoder();
    ~VideoEncoder();
    bool Initialize(ID3D11Device* pDevice, ID3D11DeviceContext* pContext,
                    int width, int height, int fps, int bitrate, bool useGpuEncoding);
    bool OpenSharedEyeTextures(const std::vector<std::pair<HANDLE, HANDLE>>& handles,
                               std::vector<ID3D11Texture2D*>& outLeft,
                               std::vector<ID3D11Texture2D*>& outRight);
    void ReleaseEyeTextures();
    void Shutdown();
    bool ComposeSBSGPU(const std::vector<ID3D11Texture2D*>& lefts,
                       const std::vector<ID3D11Texture2D*>& rights,
                       const std::vector<LayerBounds>& leftBounds,
                       const std::vector<LayerBounds>& rightBounds);
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
    bool InitializeShaderConversion();
    void CleanupShaderConversion();
    bool ComposeSBSLayer(ID3D11Texture2D* pLeft, ID3D11Texture2D* pRight,
                         const LayerBounds& leftBounds, const LayerBounds& rightBounds,
                         ID3D11BlendState* blendState);
    bool SendFrameToEncoder();
    bool ReceiveEncodedPackets();
    void LogFFmpegError(const char* context, int errorCode);
    void LogFFmpegVersion();
// D3D devices, FFmpeg contexts, shader objects, frame buffers, and encode timing counters for telemetry.
    ID3D11Device* m_pDevice;
    ID3D11DeviceContext* m_pContext;
    ID3D11Device* m_pEncDevice = nullptr;
    ID3D11DeviceContext* m_pEncContext = nullptr;
    std::vector<ID3D11Texture2D*> m_encEyeLefts;
    std::vector<ID3D11Texture2D*> m_encEyeRights;
    std::vector<std::pair<HANDLE, HANDLE>> m_openedHandles;
    AVCodecContext* m_pCodecContext;
    AVBufferRef* m_pHwDeviceCtx;
    AVFrame* m_pFrame;
    AVPacket* m_pPacket;
    AVBSFContext* m_pBsfCtx;
    SwsContext* m_pConvertContext;
    ID3D11Texture2D* m_pStagingTexture;
    bool m_initialized;
    bool m_useGpuEncoding;
    int m_width;
    int m_height;
    int m_fps;
    int m_bitrate;
    int64_t m_frameCount;
    EncodedPacketCallback m_encodedPacketCallback;
    TelemetryCallback m_telemetryCallback;
    uint8_t* m_pSoftwareFrameBuffer;
    bool m_hasValidFrame;
    ID3D11Texture2D* m_pConversionRT;
    ID3D11RenderTargetView* m_pConversionRTV;
    ID3D11VertexShader* m_pBlitVS;
    ID3D11PixelShader* m_pSBSPS;
    ID3D11SamplerState* m_pBlitSampler;
    ID3D11BlendState* m_pBlitBlend;
    ID3D11BlendState* m_pLayerBlend;
    ID3D11Buffer* m_pBoundsCB;
    ID3D11InputLayout* m_pBlitInputLayout;
    ID3D11Buffer* m_pBlitVertexBuffer;
    bool m_shaderConversionReady;
    LARGE_INTEGER m_perfFreq;
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
    uint8_t* m_pReadbackBuffer = nullptr;
    std::vector<uint8_t> m_spsPpsAnnexB;
    std::atomic<bool> m_forceKeyframe{false};
    int m_readbackBufferSize = 0;
    uint32_t m_lastLeftFmt = 0;
    uint32_t m_lastRightFmt = 0;
};
