#pragma once
#define __STDC_CONSTANT_MACROS
#include <windows.h>
#include <d3d11.h>
#include <vector>
#include <utility>
#include <functional>
#include <mutex>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVBSFContext;
struct SwsContext;
struct AVBufferRef;

class VideoEncoder
{
public:
    using EncodedPacketCallback = std::function<void(uint8_t* data, int size, int64_t pts, bool keyframe)>;

    // Per-layer valid sub-rectangle of the eye texture (VRTextureBounds_t equivalent).
    struct LayerBounds {
        float uMin = 0.0f, vMin = 0.0f, uMax = 1.0f, vMax = 1.0f;
    };

    VideoEncoder();
    ~VideoEncoder();

    bool Initialize(ID3D11Device* pDevice, ID3D11DeviceContext* pContext,
                    int width, int height, int fps, int bitrate, bool useGpuEncoding);

    // Open shared private eye copies on the encoder's own D3D11 device.
    // Must be called before ComposeSBSGPU/ReadbackToBuffer on the encoding thread.
    bool OpenSharedEyeTextures(const std::vector<std::pair<HANDLE, HANDLE>>& handles,
                               std::vector<ID3D11Texture2D*>& outLeft,
                               std::vector<ID3D11Texture2D*>& outRight);
    void ReleaseEyeTextures();
    
    void Shutdown();

    bool EncodeFrame(ID3D11Texture2D* pTexture, int64_t pts);
    bool EncodeFrameSBS(ID3D11Texture2D* pLeft, ID3D11Texture2D* pRight, int64_t pts);

    // Split-frame API for the async encode pipeline:
    // ComposeSBSGPU runs on the compositor thread while the sync texture mutex
    // is held (queues the SBS GPU pass). FinishFrame runs on the background
    // encoder thread (GPU readback + sws_scale + H264 encode + packet drain).
    bool ComposeSBSGPU(const std::vector<ID3D11Texture2D*>& lefts,
                       const std::vector<ID3D11Texture2D*>& rights,
                       const std::vector<LayerBounds>& leftBounds,
                       const std::vector<LayerBounds>& rightBounds);
    bool FinishFrame(int64_t pts);
    bool SwsConvert();                   // sws_scale BGRA→NV12 on mapped data
    bool FinishEncode(int64_t pts);      // hash + send + receive + telemetry (no D3D11)
    bool ReadBackBegin();   // D3D11: CopySubresourceRegion + Map
    bool ReadBackEnd();     // D3D11: Unmap
    bool ReadbackToBuffer();  // D3D11: CopySubresourceRegion + Map + memcpy + Unmap (all in one)

    void SetEncodedPacketCallback(EncodedPacketCallback callback);

    bool IsInitialized() const { return m_initialized; }
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }

private:
    bool InitializeFFmpeg();
    void CleanupFFmpeg();

    bool InitializeShaderConversion();
    void CleanupShaderConversion();

    bool ConvertTextureToFrame(ID3D11Texture2D* pTexture);
    bool ConvertViaShader(ID3D11Texture2D* pSource);
    // Draws one layer's eyes into the SBS conversion RT (left eye → left half,
    // right eye → right half) using the layer blend state. Caller clears the RT
    // once and sets the viewport; this is invoked once per submitted layer.
    bool ComposeSBSLayer(ID3D11Texture2D* pLeft, ID3D11Texture2D* pRight,
                         const LayerBounds& leftBounds, const LayerBounds& rightBounds,
                         ID3D11BlendState* blendState);

    void SetupBlitPipeline(ID3D11ShaderResourceView* pSRV);
    bool ReadBackConversionRT();

    bool SendFrameToEncoder();
    bool ReceiveEncodedPackets();

    void LogFFmpegError(const char* context, int errorCode);
    void LogFFmpegVersion();

    ID3D11Device* m_pDevice;
    ID3D11DeviceContext* m_pContext;

    // Second D3D11 device for encoding thread (separate from compositor's device).
    // ComposeSBSGPU + ReadbackToBuffer run on this device to avoid contention.
    ID3D11Device* m_pEncDevice = nullptr;
    ID3D11DeviceContext* m_pEncContext = nullptr;

    // Shared eye textures opened on the encoding device (one pair per layer)
    std::vector<ID3D11Texture2D*> m_encEyeLefts;
    std::vector<ID3D11Texture2D*> m_encEyeRights;

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

    uint8_t* m_pSoftwareFrameBuffer;
    bool m_hasValidFrame;

    // Shader-based format conversion (for R10G10B10A2_UNORM textures)
    ID3D11Texture2D* m_pConversionRT;
    ID3D11RenderTargetView* m_pConversionRTV;
    ID3D11VertexShader* m_pBlitVS;
    ID3D11PixelShader* m_pBlitPS;
    ID3D11PixelShader* m_pSBSPS;
    ID3D11SamplerState* m_pBlitSampler;
    ID3D11BlendState* m_pBlitBlend;
    ID3D11BlendState* m_pLayerBlend;   // alpha blend for stacking multiple layers
    ID3D11Buffer* m_pBoundsCB;         // per-layer VRTextureBounds for the SBS shader
    ID3D11InputLayout* m_pBlitInputLayout;
    ID3D11Buffer* m_pBlitVertexBuffer;
    bool m_shaderConversionReady;

    // Private staging copies for reading shared textures safely
    ID3D11Texture2D* m_pLeftStaging;
    ID3D11Texture2D* m_pRightStaging;
    ID3D11Texture2D* m_pSingleStaging;

    // Telemetry for diagnosing capture/encode stalls (image-in-image artifact)
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

    std::mutex* m_pD3dMutex = nullptr;

    // Mapped staging data between ReadBackBegin/ReadBackEnd
    void* m_mappedData = nullptr;
    UINT m_mappedRowPitch = 0;

    // CPU readback buffer (Populate in ReadbackToBuffer, consumed by SwsConvert)
    uint8_t* m_pReadbackBuffer = nullptr;

    // Annex-B formatted SPS+PPS NALs extracted from codec extradata at init.
    // Prepended to every keyframe that doesn't already start with SPS, so
    // decoders can configure even when the BSF skips the prepend.
    std::vector<uint8_t> m_spsPpsAnnexB;
    int m_readbackBufferSize = 0;
    int m_readbackRowPitch = 0;

    // DIAG (Test A): remember the eye texture formats of the last SBS compose so a
    // black-frame detection in ReadbackToBuffer can report them (H6 format check).
    uint32_t m_lastLeftFmt = 0;
    uint32_t m_lastRightFmt = 0;
};
