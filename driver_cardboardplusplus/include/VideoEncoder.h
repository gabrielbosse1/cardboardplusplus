#pragma once
#define __STDC_CONSTANT_MACROS
#include <windows.h>
#include <d3d11.h>
#include <vector>
#include <functional>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

class VideoEncoder
{
public:
    using EncodedPacketCallback = std::function<void(uint8_t* data, int size, int64_t pts, bool keyframe)>;

    VideoEncoder();
    ~VideoEncoder();

    bool Initialize(ID3D11Device* pDevice, ID3D11DeviceContext* pContext, 
                    int width, int height, int fps, int bitrate, bool useGpuEncoding);
    
    void Shutdown();

    bool EncodeFrame(ID3D11Texture2D* pTexture, int64_t pts);
    
    void SetEncodedPacketCallback(EncodedPacketCallback callback);

    bool IsInitialized() const { return m_initialized; }
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }

private:
    bool InitializeFFmpeg();
    bool InitializeHardwareEncoder();
    bool InitializeSoftwareEncoder();
    void CleanupFFmpeg();
    
    bool ConvertTextureToFrame(ID3D11Texture2D* pTexture);
    bool EncodeFrameInternal();
    bool SendFrameToEncoder();
    bool ReceiveEncodedPackets();

    void LogFFmpegError(const char* context, int errorCode);
    void LogFFmpegVersion();

    ID3D11Device* m_pDevice;
    ID3D11DeviceContext* m_pContext;
    
    AVCodecContext* m_pCodecContext;
    AVFrame* m_pFrame;
    AVPacket* m_pPacket;
    SwsContext* m_pConvertContext;
    
    ID3D11Texture2D* m_pStagingTexture;
    ID3D11Texture2D* m_pNV12TextureY;
    ID3D11Texture2D* m_pNV12TextureUV;
    ID3D11ShaderResourceView* m_pSRVY;
    ID3D11ShaderResourceView* m_pSRVUV;
    
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
};
