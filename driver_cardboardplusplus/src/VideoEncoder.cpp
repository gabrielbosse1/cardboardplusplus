#include "VideoEncoder.h"
#include "VideoEncoderFFmpeg.h"
#include "VideoEncoderLog.h"
#include <dxgi.h>
#include <dxgi1_2.h>

// ---------------------------------------------------------------------------
// VideoEncoder lifecycle + top-level frame entry points.
//
// The class is split by concern:
//   - this file:          construction, Initialize/Shutdown, shared-eye opening
//   - VideoEncoderShaders.cpp: D3D11 convert/compose pipeline (shaders + RT + readback)
//   - VideoEncoderFFmpeg.cpp:  H264 encode path (FFmpeg + swscale + telemetry)
// All private helpers are declared in VideoEncoder.h so the split is purely a
// translation-unit organization; ownership (m_pDevice / m_pCodecContext / ...)
// lives entirely inside this class.
// ---------------------------------------------------------------------------

VideoEncoder::VideoEncoder()
    : m_pDevice(nullptr)
    , m_pContext(nullptr)
    , m_pCodecContext(nullptr)
    , m_pFrame(nullptr)
    , m_pPacket(nullptr)
    , m_pBsfCtx(nullptr)
    , m_pConvertContext(nullptr)
    , m_pStagingTexture(nullptr)
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
    , m_pConversionRT(nullptr)
    , m_pConversionRTV(nullptr)
    , m_pBlitVS(nullptr)
    , m_pBlitPS(nullptr)
    , m_pBlitSampler(nullptr)
    , m_pBlitBlend(nullptr)
    , m_pLayerBlend(nullptr)
    , m_pBoundsCB(nullptr)
    , m_pBlitInputLayout(nullptr)
    , m_pBlitVertexBuffer(nullptr)
    , m_shaderConversionReady(false)
    , m_pLeftStaging(nullptr)
    , m_pRightStaging(nullptr)
    , m_pSingleStaging(nullptr)
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
    QueryPerformanceFrequency(&m_perfFreq);
}

VideoEncoder::~VideoEncoder()
{
    Shutdown();
}

bool VideoEncoder::Initialize(ID3D11Device* pDevice, ID3D11DeviceContext* pContext,
                             int width, int height, int fps, int bitrate, bool useGpuEncoding)
{
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

    if (!pDevice || !pContext) {
        ENCODER_ERROR("Invalid D3D11 device or context!");
        return false;
    }

    if (width <= 0 || height <= 0) {
        ENCODER_ERROR("Invalid dimensions: %dx%d", width, height);
        return false;
    }

    m_pDevice = pDevice;
    m_pContext = pContext;
    m_width = width;
    m_height = height;
    m_fps = fps;
    m_bitrate = bitrate;
    m_useGpuEncoding = useGpuEncoding;
    m_frameCount = 0;

    // Create a SECOND D3D11 device for the encoding thread. This device
    // is independent from the compositor's device, so the encoding thread
    // can use D3D11 without contention while Present() runs on device 1.
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr2 = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        0, nullptr, 0, D3D11_SDK_VERSION,
        &m_pEncDevice, &featureLevel, &m_pEncContext);
    if (FAILED(hr2)) {
        ENCODER_ERROR("Failed to create encoding D3D11 device! HRESULT: 0x%x", hr2);
        ENCODER_ERROR("Falling back to compositor device (will cause contention)");
        m_pEncDevice = nullptr;
        m_pEncContext = nullptr;
    } else {
        ENCODER_LOG("Created second D3D11 device for encoding thread (feature level: 0x%x)", featureLevel);
    }

    // Point GPU resources at the encoding device if available, else compositor device.
    ID3D11Device* gpuDevice = m_pEncDevice ? m_pEncDevice : m_pDevice;
    ID3D11DeviceContext* gpuContext = m_pEncContext ? m_pEncContext : m_pContext;
    m_pDevice = gpuDevice;
    m_pContext = gpuContext;

    LogFFmpegVersion();

    if (!InitializeFFmpeg()) {
        ENCODER_ERROR("Failed to initialize FFmpeg!");
        CleanupFFmpeg();
        return false;
    }

    if (!InitializeShaderConversion()) {
        ENCODER_ERROR("Failed to initialize shader conversion!");
        CleanupShaderConversion();
        CleanupFFmpeg();
        return false;
    }

    m_initialized = true;
    ENCODER_LOG("VideoEncoder initialized successfully!");
    return true;
}

void VideoEncoder::Shutdown()
{
    if (!m_initialized && !m_pCodecContext && !m_pFrame && !m_pPacket) {
        return;
    }

    ENCODER_LOG("Shutting down VideoEncoder...");

    CleanupShaderConversion();
    CleanupFFmpeg();

    if (m_pStagingTexture) {
        m_pStagingTexture->Release();
        m_pStagingTexture = nullptr;
    }

    if (m_pSoftwareFrameBuffer) {
        av_free(m_pSoftwareFrameBuffer);
        m_pSoftwareFrameBuffer = nullptr;
    }

    ReleaseEyeTextures();

    if (m_pEncContext) { m_pEncContext->Release(); m_pEncContext = nullptr; }
    if (m_pEncDevice) { m_pEncDevice->Release(); m_pEncDevice = nullptr; }

    m_initialized = false;
    ENCODER_LOG("VideoEncoder shutdown complete.");
}

bool VideoEncoder::OpenSharedEyeTextures(const std::vector<std::pair<HANDLE, HANDLE>>& handles,
                                           std::vector<ID3D11Texture2D*>& outLeft,
                                           std::vector<ID3D11Texture2D*>& outRight)
{
    ReleaseEyeTextures();

    ID3D11Device* dev = m_pEncDevice ? m_pEncDevice : m_pDevice;
    if (!dev || handles.empty()) return false;

    m_encEyeLefts.reserve(handles.size());
    m_encEyeRights.reserve(handles.size());
    for (const auto& h : handles) {
        if (!h.first || !h.second) return false;
        ID3D11Texture2D* pL = nullptr;
        ID3D11Texture2D* pR = nullptr;
        HRESULT hr = dev->OpenSharedResource(h.first, __uuidof(ID3D11Texture2D), (void**)&pL);
        if (FAILED(hr)) {
            ENCODER_ERROR("OpenSharedResource(left) failed! HRESULT: 0x%x", hr);
            return false;
        }
        hr = dev->OpenSharedResource(h.second, __uuidof(ID3D11Texture2D), (void**)&pR);
        if (FAILED(hr)) {
            ENCODER_ERROR("OpenSharedResource(right) failed! HRESULT: 0x%x", hr);
            pL->Release();
            return false;
        }
        m_encEyeLefts.push_back(pL);
        m_encEyeRights.push_back(pR);
    }

    outLeft = m_encEyeLefts;
    outRight = m_encEyeRights;
    return true;
}

void VideoEncoder::ReleaseEyeTextures()
{
    for (auto* t : m_encEyeLefts) { if (t) t->Release(); }
    for (auto* t : m_encEyeRights) { if (t) t->Release(); }
    m_encEyeLefts.clear();
    m_encEyeRights.clear();
}

bool VideoEncoder::EncodeFrame(ID3D11Texture2D* pTexture, int64_t pts)
{
    if (!m_initialized || !pTexture) {
        ENCODER_ERROR("EncodeFrame called without valid encoder or texture!");
        return false;
    }

    if (!ConvertTextureToFrame(pTexture)) {
        ENCODER_ERROR("Failed to convert texture to frame!");
        return false;
    }

    m_pFrame->pts = pts;
    m_hasValidFrame = true;

    if (!SendFrameToEncoder()) {
        ENCODER_ERROR("Failed to send frame to encoder!");
        return false;
    }

    if (!ReceiveEncodedPackets()) {
        ENCODER_ERROR("Failed to receive encoded packets!");
        return false;
    }

    return true;
}

bool VideoEncoder::EncodeFrameSBS(ID3D11Texture2D* pLeft, ID3D11Texture2D* pRight, int64_t pts)
{
    if (!ComposeSBSGPU({ pLeft }, { pRight }, { LayerBounds{} }, { LayerBounds{} })) {
        return false;
    }

    return FinishFrame(pts);
}

void VideoEncoder::SetEncodedPacketCallback(EncodedPacketCallback callback)
{
    m_encodedPacketCallback = callback;
}