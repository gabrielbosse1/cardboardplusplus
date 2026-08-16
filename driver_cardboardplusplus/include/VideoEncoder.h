#pragma once
#define __STDC_CONSTANT_MACROS
#include <windows.h>
#include <d3d11.h>
#include <vector>
#include <functional>

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

    VideoEncoder();
    ~VideoEncoder();

    bool Initialize(ID3D11Device* pDevice, ID3D11DeviceContext* pContext,
                    int width, int height, int fps, int bitrate);
    
    void Shutdown();

    bool EncodeFrame(ID3D11Texture2D* pTexture, int64_t pts);
    bool EncodeFrameSBS(ID3D11Texture2D* pLeft, ID3D11Texture2D* pRight, int64_t pts);

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
    bool ComposeSBS(ID3D11Texture2D* pLeft, ID3D11Texture2D* pRight);

    void SetupBlitPipeline(ID3D11ShaderResourceView* pSRV);
    bool ReadBackConversionRT();

    bool SendFrameToEncoder();
    bool ReceiveEncodedPackets();

    void LogFFmpegError(const char* context, int errorCode);
    void LogFFmpegVersion();

    ID3D11Device* m_pDevice;
    ID3D11DeviceContext* m_pContext;

    AVCodecContext* m_pCodecContext;
    AVBufferRef* m_pHwDeviceCtx;
    AVFrame* m_pFrame[2];
    AVPacket* m_pPacket;
    AVBSFContext* m_pBsfCtx;
    SwsContext* m_pConvertContext;

    ID3D11Texture2D* m_pStagingTexture[2];

    bool m_initialized;
    int m_width;
    int m_height;
    int m_fps;
    int m_bitrate;
    int64_t m_frameCount;

    EncodedPacketCallback m_encodedPacketCallback;

    uint8_t* m_pSoftwareFrameBuffer[2];
    int m_bufferIndex = 0;
    bool m_hasValidFrame;

    // Shader-based format conversion (for R10G10B10A2_UNORM textures)
    ID3D11Texture2D* m_pConversionRT;
    ID3D11RenderTargetView* m_pConversionRTV;
    ID3D11VertexShader* m_pBlitVS;
    ID3D11PixelShader* m_pBlitPS;
    ID3D11PixelShader* m_pSBSPS;
    ID3D11SamplerState* m_pBlitSampler;
    ID3D11BlendState* m_pBlitBlend;
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
};
