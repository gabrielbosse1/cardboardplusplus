#include "VideoEncoder.h"
#include "HmdDriver.h"
#include <dxgi.h>
#include <dxgi1_2.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

static void DriverLogFFmpeg(const char* pFormat, ...) {
    char buffer[2048];
    va_list args;
    va_start(args, pFormat);
    vsprintf_s(buffer, pFormat, args);
    va_end(args);
    strcat_s(buffer, "\n");
    vr::VRDriverLog()->Log(buffer);
}

#define ENCODER_LOG(...) DriverLogFFmpeg("[VideoEncoder] " __VA_ARGS__)
#define ENCODER_ERROR(...) DriverLogFFmpeg("[VideoEncoder] ERROR: " __VA_ARGS__)

VideoEncoder::VideoEncoder()
    : m_pDevice(nullptr)
    , m_pContext(nullptr)
    , m_pCodecContext(nullptr)
    , m_pFrame(nullptr)
    , m_pPacket(nullptr)
    , m_pConvertContext(nullptr)
    , m_pStagingTexture(nullptr)
    , m_pNV12TextureY(nullptr)
    , m_pNV12TextureUV(nullptr)
    , m_pSRVY(nullptr)
    , m_pSRVUV(nullptr)
    , m_initialized(false)
    , m_useGpuEncoding(false)
    , m_width(0)
    , m_height(0)
    , m_fps(0)
    , m_bitrate(0)
    , m_frameCount(0)
    , m_pSoftwareFrameBuffer(nullptr)
    , m_hasValidFrame(false)
{
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

    LogFFmpegVersion();

    if (!InitializeFFmpeg()) {
        ENCODER_ERROR("Failed to initialize FFmpeg!");
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

    CleanupFFmpeg();

    if (m_pStagingTexture) {
        m_pStagingTexture->Release();
        m_pStagingTexture = nullptr;
    }

    if (m_pNV12TextureY) {
        m_pNV12TextureY->Release();
        m_pNV12TextureY = nullptr;
    }

    if (m_pNV12TextureUV) {
        m_pNV12TextureUV->Release();
        m_pNV12TextureUV = nullptr;
    }

    if (m_pSRVY) {
        m_pSRVY->Release();
        m_pSRVY = nullptr;
    }

    if (m_pSRVUV) {
        m_pSRVUV->Release();
        m_pSRVUV = nullptr;
    }

    if (m_pSoftwareFrameBuffer) {
        av_free(m_pSoftwareFrameBuffer);
        m_pSoftwareFrameBuffer = nullptr;
    }

    m_initialized = false;
    ENCODER_LOG("VideoEncoder shutdown complete.");
}

bool VideoEncoder::InitializeFFmpeg()
{
    ENCODER_LOG("Initializing FFmpeg H264 encoder...");

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        ENCODER_ERROR("H264 encoder not found! Is libx264 compiled in FFmpeg?");
        ENCODER_ERROR("Trying alternative encoders...");
        
        codec = avcodec_find_encoder_by_name("libx264");
        if (!codec) {
            codec = avcodec_find_encoder_by_name("h264_nvenc");
            if (!codec) {
                codec = avcodec_find_encoder_by_name("h264_qsv");
                if (!codec) {
                    ENCODER_ERROR("No H264 encoder available!");
                    return false;
                } else {
                    ENCODER_LOG("Found QSV H264 encoder");
                }
            } else {
                ENCODER_LOG("Found NVIDIA NVENC H264 encoder");
            }
        } else {
            ENCODER_LOG("Found libx264 H264 encoder");
        }
    } else {
        ENCODER_LOG("Found H264 encoder: %s", codec->name);
    }

    ENCODER_LOG("Encoder name: %s, long name: %s", 
                codec->name, codec->long_name ? codec->long_name : "N/A");

    m_pCodecContext = avcodec_alloc_context3(codec);
    if (!m_pCodecContext) {
        ENCODER_ERROR("Failed to allocate codec context!");
        return false;
    }

    m_pCodecContext->width = m_width;
    m_pCodecContext->height = m_height;
    m_pCodecContext->time_base = { 1, m_fps };
    m_pCodecContext->framerate = { m_fps, 1 };
    m_pCodecContext->gop_size = 30;
    m_pCodecContext->max_b_frames = 0;
    m_pCodecContext->pix_fmt = AV_PIX_FMT_NV12;

    av_opt_set(m_pCodecContext->priv_data, "preset", "fast", 0);
    av_opt_set(m_pCodecContext->priv_data, "tune", "zerolatency", 0);
    av_opt_set(m_pCodecContext->priv_data, "profile", "baseline", 0);
    av_opt_set_int(m_pCodecContext->priv_data, "rc", 0, 0);
    
    m_pCodecContext->bit_rate = m_bitrate;
    m_pCodecContext->rc_min_rate = m_bitrate;
    m_pCodecContext->rc_max_rate = m_bitrate;
    m_pCodecContext->rc_buffer_size = m_bitrate / 2;
    m_pCodecContext->delay = 0;

    ENCODER_LOG("Codec config - bitrate: %d, gop: %d, max_b_frames: %d",
                m_pCodecContext->bit_rate, m_pCodecContext->gop_size, m_pCodecContext->max_b_frames);

    if (m_useGpuEncoding) {
        ENCODER_LOG("Attempting GPU-accelerated encoding...");
        
        m_pCodecContext->pix_fmt = AV_PIX_FMT_CUDA;
        
        int ret = av_hwdevice_ctx_create(&m_pCodecContext->hw_device_ctx, 
                                        AV_HWDEVICE_TYPE_CUDA, 
                                        nullptr, nullptr, 0);
        if (ret < 0) {
            ENCODER_ERROR("Failed to create CUDA device context! Error code: %d", ret);
            ENCODER_ERROR("Falling back to software encoding...");
            m_useGpuEncoding = false;
            m_pCodecContext->pix_fmt = AV_PIX_FMT_NV12;
            m_pCodecContext->hw_device_ctx = nullptr;
        } else {
            ENCODER_LOG("CUDA device context created successfully!");
        }
    }

    if (!m_useGpuEncoding) {
        ENCODER_LOG("Using software encoding with NV12 pix_fmt...");
        m_pCodecContext->pix_fmt = AV_PIX_FMT_NV12;
        
        D3D11_TEXTURE2D_DESC stagingDesc = {};
        stagingDesc.Width = m_width;
        stagingDesc.Height = m_height;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        HRESULT hr = m_pDevice->CreateTexture2D(&stagingDesc, nullptr, &m_pStagingTexture);
        if (FAILED(hr)) {
            ENCODER_ERROR("Failed to create staging texture! HRESULT: 0x%x", hr);
            return false;
        }
        ENCODER_LOG("Staging texture created successfully");

        D3D11_TEXTURE2D_DESC nv12Desc = {};
        nv12Desc.Width = m_width;
        nv12Desc.Height = m_height;
        nv12Desc.MipLevels = 1;
        nv12Desc.ArraySize = 1;
        nv12Desc.Format = DXGI_FORMAT_NV12;
        nv12Desc.SampleDesc.Count = 1;
        nv12Desc.Usage = D3D11_USAGE_DEFAULT;
        nv12Desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        hr = m_pDevice->CreateTexture2D(&nv12Desc, nullptr, &m_pNV12TextureY);
        if (FAILED(hr)) {
            ENCODER_ERROR("Failed to create NV12 texture! HRESULT: 0x%x", hr);
            return false;
        }
        ENCODER_LOG("NV12 texture created successfully");

        m_pConvertContext = sws_getContext(
            m_width, m_height, AV_PIX_FMT_BGRA,
            m_width, m_height, AV_PIX_FMT_NV12,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
        );
        
        if (!m_pConvertContext) {
            ENCODER_ERROR("Failed to create SWScale context!");
            return false;
        }
        ENCODER_LOG("SWScale context created for BGRA -> NV12 conversion");

        m_pSoftwareFrameBuffer = (uint8_t*)av_malloc(m_width * m_height * 4);
        if (!m_pSoftwareFrameBuffer) {
            ENCODER_ERROR("Failed to allocate software frame buffer!");
            return false;
        }
    }

    int ret = avcodec_open2(m_pCodecContext, codec, nullptr);
    if (ret < 0) {
        ENCODER_ERROR("Failed to open codec! Error code: %d", ret);
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        ENCODER_ERROR("Error message: %s", errbuf);
        return false;
    }
    ENCODER_LOG("Codec opened successfully!");
    ENCODER_LOG("  Selected pix_fmt: %d (%s)", 
                m_pCodecContext->pix_fmt,
                av_get_pix_fmt_name(m_pCodecContext->pix_fmt));

    m_pFrame = av_frame_alloc();
    if (!m_pFrame) {
        ENCODER_ERROR("Failed to allocate frame!");
        return false;
    }

    if (m_useGpuEncoding) {
        m_pFrame->format = AV_PIX_FMT_CUDA;
    } else {
        m_pFrame->format = AV_PIX_FMT_NV12;
        m_pFrame->data[0] = m_pSoftwareFrameBuffer;
        m_pFrame->linesize[0] = m_width;
        m_pFrame->data[1] = m_pSoftwareFrameBuffer + m_width * m_height;
        m_pFrame->linesize[1] = m_width;
    }
    m_pFrame->width = m_width;
    m_pFrame->height = m_height;

    m_pPacket = av_packet_alloc();
    if (!m_pPacket) {
        ENCODER_ERROR("Failed to allocate packet!");
        return false;
    }

    ENCODER_LOG("FFmpeg H264 encoder initialized successfully!");
    ENCODER_LOG("  Codec: %s", codec->name);
    ENCODER_LOG("  Resolution: %dx%d", m_width, m_height);
    ENCODER_LOG("  FPS: %d", m_fps);
    ENCODER_LOG("  Bitrate: %d bps", m_bitrate);

    return true;
}

void VideoEncoder::CleanupFFmpeg()
{
    ENCODER_LOG("Cleaning up FFmpeg resources...");

    if (m_pPacket) {
        av_packet_free(&m_pPacket);
        m_pPacket = nullptr;
    }

    if (m_pFrame) {
        av_frame_free(&m_pFrame);
        m_pFrame = nullptr;
    }

    if (m_pConvertContext) {
        sws_freeContext(m_pConvertContext);
        m_pConvertContext = nullptr;
    }

    if (m_pCodecContext) {
        avcodec_free_context(&m_pCodecContext);
        m_pCodecContext = nullptr;
    }

    ENCODER_LOG("FFmpeg resources cleaned up.");
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

bool VideoEncoder::ConvertTextureToFrame(ID3D11Texture2D* pTexture)
{
    D3D11_TEXTURE2D_DESC desc;
    pTexture->GetDesc(&desc);

    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        ENCODER_ERROR("Unsupported texture format: %d", desc.Format);
        return false;
    }

    ENCODER_LOG("Converting texture %dx%d to frame...", desc.Width, desc.Height);

    m_pContext->CopySubresourceRegion(m_pStagingTexture, 0, 0, 0, 0, pTexture, 0, nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_pContext->Map(m_pStagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        ENCODER_ERROR("Failed to map staging texture! HRESULT: 0x%x", hr);
        return false;
    }

    uint8_t* srcSlice[1] = { (uint8_t*)mapped.pData };
    int srcStride[1] = { static_cast<int>(mapped.RowPitch) };

    uint8_t* dstSlice[4] = { m_pSoftwareFrameBuffer, m_pSoftwareFrameBuffer + m_width * m_height };
    int dstStride[4] = { m_width, m_width };

    int ret = sws_scale(m_pConvertContext, srcSlice, srcStride, 0, m_height,
                        dstSlice, dstStride);

    m_pContext->Unmap(m_pStagingTexture, 0);

    if (ret != m_height) {
        ENCODER_ERROR("swscale conversion failed! Returned %d, expected %d", ret, m_height);
        return false;
    }

    ENCODER_LOG("Texture converted successfully to NV12");
    return true;
}

bool VideoEncoder::SendFrameToEncoder()
{
    int ret = avcodec_send_frame(m_pCodecContext, m_pFrame);
    if (ret < 0) {
        ENCODER_ERROR("avcodec_send_frame failed! Error code: %d", ret);
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        ENCODER_ERROR("Error: %s", errbuf);
        return false;
    }

    m_hasValidFrame = false;
    return true;
}

bool VideoEncoder::ReceiveEncodedPackets()
{
    int ret;
    while ((ret = avcodec_receive_packet(m_pCodecContext, m_pPacket)) >= 0) {
        bool keyframe = (m_pPacket->flags & AV_PKT_FLAG_KEY) != 0;
        
        ENCODER_LOG("Encoded packet: size=%d, pts=%lld, keyframe=%s",
                    m_pPacket->size, m_pPacket->pts, keyframe ? "YES" : "NO");

        if (m_encodedPacketCallback) {
            m_encodedPacketCallback(m_pPacket->data, m_pPacket->size, 
                                    m_pPacket->pts, keyframe);
        }

        av_packet_unref(m_pPacket);
    }

    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        LogFFmpegError("avcodec_receive_packet", ret);
        return false;
    }

    return true;
}

void VideoEncoder::SetEncodedPacketCallback(EncodedPacketCallback callback)
{
    m_encodedPacketCallback = callback;
}

void VideoEncoder::LogFFmpegError(const char* context, int errorCode)
{
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(errorCode, errbuf, sizeof(errbuf));
    ENCODER_ERROR("%s failed with error %d: %s", context, errorCode, errbuf);
}

void VideoEncoder::LogFFmpegVersion()
{
    ENCODER_LOG("FFmpeg version info:");
    ENCODER_LOG("  libavcodec version: %d.%d.%d",
                AV_VERSION_MAJOR(avcodec_version()),
                AV_VERSION_MINOR(avcodec_version()),
                AV_VERSION_MICRO(avcodec_version()));
    ENCODER_LOG("  libavformat version: %d.%d.%d",
                AV_VERSION_MAJOR(avformat_version()),
                AV_VERSION_MINOR(avformat_version()),
                AV_VERSION_MICRO(avformat_version()));
    ENCODER_LOG("  libavutil version: %d.%d.%d",
                AV_VERSION_MAJOR(avutil_version()),
                AV_VERSION_MINOR(avutil_version()),
                AV_VERSION_MICRO(avutil_version()));
    ENCODER_LOG("  libswscale version: %d.%d.%d",
                AV_VERSION_MAJOR(swscale_version()),
                AV_VERSION_MINOR(swscale_version()),
                AV_VERSION_MICRO(swscale_version()));
}
