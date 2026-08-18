#include "VideoEncoder.h"
#include "HmdDriver.h"
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <cstring>
#pragma comment(lib, "d3dcompiler.lib")

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/mem.h>
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

// Fullscreen triangle vertex shader - outputs SV_Position and computed UV
static const char* kBlitVSSource = R"(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut main(uint id : SV_VertexID) {
    VSOut o;
    float2 xy = float2((id << 1) & 2, id & 2);
    o.pos = float4(xy * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = xy * float2(1, 1);  // (0,0)=top-left, (1,0)=top-right, (0,1)=bottom-left
    return o;
}
)";

// Pixel shader with UV-based sampling and linear-to-sRGB conversion
static const char* kBlitPSSource = R"(
Texture2D<float4> src : register(t0);
SamplerState samp : register(s0);

struct PSIn {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 linearToSrgb(float4 c) {
    float3 lo = 12.92 * c.rgb;
    float3 hi = 1.055 * pow(c.rgb, 1.0/2.4) - 0.055;
    c.rgb = (c.rgb <= 0.0031308) ? lo : hi;
    return saturate(c);
}

float4 main(PSIn input) : SV_Target {
    float4 c = src.Sample(samp, input.uv);
    return linearToSrgb(c);
}
)";

// SBS compose pixel shader - chooses left or right eye based on x coordinate.
// Each layer draws with its own source sub-rect (VRTextureBounds) so partial
// overlays are placed correctly. leftRect/rightRect = (offsetU, offsetV, scaleU, scaleV).
static const char* kSBSPSSource = R"(
Texture2D<float4> leftEye : register(t0);
Texture2D<float4> rightEye : register(t1);
SamplerState samp : register(s0);
cbuffer Bounds : register(b0) {
    float4 leftRect;   // x=offsetU, y=offsetV, z=scaleU, w=scaleV
    float4 rightRect;
};

struct PSIn {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 linearToSrgb(float4 c) {
    float3 lo = 12.92 * c.rgb;
    float3 hi = 1.055 * pow(c.rgb, 1.0/2.4) - 0.055;
    c.rgb = (c.rgb <= 0.0031308) ? lo : hi;
    return saturate(c);
}

float4 main(PSIn input) : SV_Target {
    float4 c;
    if (input.uv.x < 0.5) {
        float2 leftUV = leftRect.xy + float2(input.uv.x * 2.0, input.uv.y) * leftRect.zw;
        c = leftEye.Sample(samp, leftUV);
    } else {
        float2 rightUV = rightRect.xy + float2((input.uv.x - 0.5) * 2.0, input.uv.y) * rightRect.zw;
        c = rightEye.Sample(samp, rightUV);
    }
    return linearToSrgb(c);
}
)";

// Helper: compile shader from string
static ID3DBlob* CompileShader(const char* source, const char* name, const char* target) {
    ID3DBlob* blob = nullptr;
    ID3DBlob* errorBlob = nullptr;
    HRESULT hr = D3DCompile(source, strlen(source), name,
                            nullptr, nullptr, "main", target, 0, 0, &blob, &errorBlob);
    if (FAILED(hr)) {
        ENCODER_ERROR("Failed to compile %s! HRESULT: 0x%x", name, hr);
        if (errorBlob) {
            ENCODER_ERROR("Shader error: %s", (char*)errorBlob->GetBufferPointer());
            errorBlob->Release();
        }
        if (blob) blob->Release();
        return nullptr;
    }
    return blob;
}

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

bool VideoEncoder::InitializeFFmpeg()
{
    ENCODER_LOG("Initializing FFmpeg H264 encoder...");

    // Prefer hardware (GPU) encoders for throughput; fall back to libx264.
    // h264_amf = AMD, h264_nvenc = NVIDIA, h264_qsv = Intel.
    const char* codecCandidates[] = { "h264_amf", "h264_nvenc", "h264_qsv", "libx264" };
    const AVCodec* codec = nullptr;

    for (const char* name : codecCandidates) {
        const AVCodec* c = avcodec_find_encoder_by_name(name);
        if (!c) {
            ENCODER_LOG("Encoder %s not available, skipping", name);
            continue;
        }

        m_pCodecContext = avcodec_alloc_context3(c);
        if (!m_pCodecContext) {
            ENCODER_ERROR("Failed to allocate codec context for %s!", name);
            continue;
        }

        m_pCodecContext->width = m_width;
        m_pCodecContext->height = m_height;
        m_pCodecContext->time_base = { 1, m_fps };
        m_pCodecContext->framerate = { m_fps, 1 };
        m_pCodecContext->gop_size = 10;
        m_pCodecContext->max_b_frames = 0;
        m_pCodecContext->delay = 0;
        m_pCodecContext->pix_fmt = AV_PIX_FMT_NV12;
        m_pCodecContext->bit_rate = m_bitrate;
        m_pCodecContext->rc_min_rate = m_bitrate;
        m_pCodecContext->rc_max_rate = m_bitrate;
        m_pCodecContext->rc_buffer_size = m_bitrate / 2;
        // Force SPS/PPS into extradata (AVCC): hardware encoders (AMF/NVENC/
        // QSV) otherwise emit length-prefixed NALs without any start codes,
        // which the phone's decoder cannot parse. The h264_mp4toannexb BSF
        // below restores a start-code (Annex B) stream for the wire.
        m_pCodecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        bool isLibx264 = (strcmp(name, "libx264") == 0);
        bool isAmf     = (strcmp(name, "h264_amf") == 0);

        if (isAmf) {
            // AMF requires a D3D11 hardware-device context. Wrap the driver's
            // existing ID3D11Device so AMF encodes on the same GPU (no extra device).
            // Note: av_hwdevice_ctx_alloc() returns a buffer whose data is an
            // AVHWDeviceContext; the D3D11VA-specific context is at hwctx->hwctx.
            AVBufferRef* hwdev = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
            if (hwdev) {
                AVD3D11VADeviceContext* d3d11va =
                    (AVD3D11VADeviceContext*)((AVHWDeviceContext*)hwdev->data)->hwctx;
                d3d11va->device = m_pDevice;
                m_pDevice->AddRef();
                if (av_hwdevice_ctx_init(hwdev) == 0) {
                    m_pHwDeviceCtx = hwdev;
                    m_pCodecContext->hw_device_ctx = av_buffer_ref(hwdev);
                    ENCODER_LOG("Wrapped driver ID3D11Device for AMF");
                } else {
                    ENCODER_ERROR("Failed to init D3D11VA hw device for AMF");
                    av_buffer_unref(&hwdev);
                }
            } else {
                ENCODER_ERROR("Failed to allocate D3D11VA hw device for AMF");
            }

            // AMF low-latency options (unknown names are ignored safely).
            av_opt_set(m_pCodecContext->priv_data, "usage", "ultralowlatency", 0);
            av_opt_set(m_pCodecContext->priv_data, "rc", "cbr", 0);
            av_opt_set(m_pCodecContext->priv_data, "profile", "baseline", 0);
        } else if (isLibx264) {
            av_opt_set(m_pCodecContext->priv_data, "preset", "ultrafast", 0);
            av_opt_set(m_pCodecContext->priv_data, "tune", "zerolatency", 0);
            av_opt_set(m_pCodecContext->priv_data, "profile", "baseline", 0);
        } else {
            // h264_nvenc / h264_qsv: vendor-valid preset + baseline profile.
            const char* hwPreset = (strcmp(name, "h264_nvenc") == 0) ? "fast" : "veryfast";
            av_opt_set(m_pCodecContext->priv_data, "preset", hwPreset, 0);
            av_opt_set(m_pCodecContext->priv_data, "profile", "baseline", 0);
        }

        int ret = avcodec_open2(m_pCodecContext, c, nullptr);
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            ENCODER_ERROR("Failed to open codec %s: %s", name, errbuf);
            if (m_pCodecContext->hw_device_ctx) {
                av_buffer_unref(&m_pCodecContext->hw_device_ctx);
            }
            if (m_pHwDeviceCtx) {
                av_buffer_unref(&m_pHwDeviceCtx);
            }
            avcodec_free_context(&m_pCodecContext);
            m_pCodecContext = nullptr;
            continue;
        }

        codec = c;
        m_useGpuEncoding = !isLibx264;
        ENCODER_LOG("Opened H264 encoder: %s (%s)", name,
                    m_useGpuEncoding ? "hardware/GPU" : "software");
        break;
    }

    if (!codec || !m_pCodecContext) {
        ENCODER_ERROR("No usable H264 encoder found!");
        return false;
    }

    // With AV_CODEC_FLAG_GLOBAL_HEADER the encoder emits length-prefixed
    // (AVCC) packets + avcC extradata. Convert to Annex B (start codes) and
    // re-insert SPS/PPS in-band so the phone's decoder can configure itself.
    m_pBsfCtx = nullptr;
    if (m_pCodecContext->extradata && m_pCodecContext->extradata_size > 0) {
        const AVBitStreamFilter* bsf = av_bsf_get_by_name("h264_mp4toannexb");
        if (bsf) {
            int bret = av_bsf_alloc(bsf, &m_pBsfCtx);
            if (bret >= 0) {
                AVCodecParameters* par = m_pBsfCtx->par_in;
                par->codec_id = AV_CODEC_ID_H264;
                par->extradata = (uint8_t*)av_memdup(m_pCodecContext->extradata,
                                                     m_pCodecContext->extradata_size);
                par->extradata_size = m_pCodecContext->extradata_size;
                bret = av_bsf_init(m_pBsfCtx);
                if (bret >= 0) {
                    ENCODER_LOG("h264_mp4toannexb BSF enabled (Annex B on the wire)");
                } else {
                    av_bsf_free(&m_pBsfCtx);
                    m_pBsfCtx = nullptr;
                    ENCODER_ERROR("Failed to init h264_mp4toannexb BSF!");
                }
            } else {
                ENCODER_ERROR("Failed to allocate h264_mp4toannexb BSF!");
            }
        }
    } else {
        ENCODER_LOG("No encoder extradata available; BSF skipped");
    }

    ENCODER_LOG("Encoder name: %s, long name: %s",
                codec->name, codec->long_name ? codec->long_name : "N/A");

    // Create staging texture for CPU readback (BGRA8)
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

    // Create swscale context for BGRA -> NV12 conversion
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

    ENCODER_LOG("Codec opened successfully!");
    ENCODER_LOG("  Selected pix_fmt: %d (%s)",
                m_pCodecContext->pix_fmt,
                av_get_pix_fmt_name(m_pCodecContext->pix_fmt));

    m_pFrame = av_frame_alloc();
    if (!m_pFrame) {
        ENCODER_ERROR("Failed to allocate frame!");
        return false;
    }

    m_pFrame->format = AV_PIX_FMT_NV12;
    m_pFrame->data[0] = m_pSoftwareFrameBuffer;
    m_pFrame->linesize[0] = m_width;
    m_pFrame->data[1] = m_pSoftwareFrameBuffer + m_width * m_height;
    m_pFrame->linesize[1] = m_width;
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

    if (m_pBsfCtx) {
        av_bsf_free(&m_pBsfCtx);
        m_pBsfCtx = nullptr;
    }

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

    if (m_pHwDeviceCtx) {
        av_buffer_unref(&m_pHwDeviceCtx);
        m_pHwDeviceCtx = nullptr;
    }

    ENCODER_LOG("FFmpeg resources cleaned up.");
}

bool VideoEncoder::InitializeShaderConversion()
{
    ENCODER_LOG("Initializing shader-based texture conversion...");

    // Create conversion render target (BGRA8 at encoder resolution)
    D3D11_TEXTURE2D_DESC rtDesc = {};
    rtDesc.Width = m_width;
    rtDesc.Height = m_height;
    rtDesc.MipLevels = 1;
    rtDesc.ArraySize = 1;
    rtDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rtDesc.SampleDesc.Count = 1;
    rtDesc.Usage = D3D11_USAGE_DEFAULT;
    rtDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = m_pDevice->CreateTexture2D(&rtDesc, nullptr, &m_pConversionRT);
    if (FAILED(hr)) {
        ENCODER_ERROR("Failed to create conversion render target! HRESULT: 0x%x", hr);
        return false;
    }

    hr = m_pDevice->CreateRenderTargetView(m_pConversionRT, nullptr, &m_pConversionRTV);
    if (FAILED(hr)) {
        ENCODER_ERROR("Failed to create conversion RTV! HRESULT: 0x%x", hr);
        return false;
    }
    ENCODER_LOG("Conversion render target created: %dx%d", m_width, m_height);

    // Compile vertex shader
    ID3DBlob* vsBlob = CompileShader(kBlitVSSource, "BlitVS", "vs_5_0");
    if (!vsBlob) return false;

    hr = m_pDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                        nullptr, &m_pBlitVS);
    if (FAILED(hr)) {
        ENCODER_ERROR("Failed to create vertex shader! HRESULT: 0x%x", hr);
        vsBlob->Release();
        return false;
    }

    // Compile blit pixel shader
    ID3DBlob* psBlob = CompileShader(kBlitPSSource, "BlitPS", "ps_5_0");
    if (!psBlob) { vsBlob->Release(); return false; }

    hr = m_pDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                       nullptr, &m_pBlitPS);
    if (FAILED(hr)) {
        ENCODER_ERROR("Failed to create pixel shader! HRESULT: 0x%x", hr);
        vsBlob->Release();
        psBlob->Release();
        return false;
    }

    // Compile SBS pixel shader
    ID3DBlob* sbsPsBlob = CompileShader(kSBSPSSource, "SBSPS", "ps_5_0");
    if (!sbsPsBlob) { vsBlob->Release(); psBlob->Release(); return false; }

    hr = m_pDevice->CreatePixelShader(sbsPsBlob->GetBufferPointer(), sbsPsBlob->GetBufferSize(),
                                       nullptr, &m_pSBSPS);
    sbsPsBlob->Release();
    if (FAILED(hr)) {
        ENCODER_ERROR("Failed to create SBS pixel shader! HRESULT: 0x%x", hr);
        vsBlob->Release();
        psBlob->Release();
        return false;
    }

    // Create sampler state (linear clamp)
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;

    hr = m_pDevice->CreateSamplerState(&sampDesc, &m_pBlitSampler);
    if (FAILED(hr)) {
        ENCODER_ERROR("Failed to create sampler state! HRESULT: 0x%x", hr);
        vsBlob->Release();
        psBlob->Release();
        return false;
    }

    // Create blend state (no blending) — used by the simple blit/convert paths.
    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = FALSE;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    hr = m_pDevice->CreateBlendState(&blendDesc, &m_pBlitBlend);
    if (FAILED(hr)) {
        ENCODER_ERROR("Failed to create blend state! HRESULT: 0x%x", hr);
        vsBlob->Release();
        psBlob->Release();
        return false;
    }

    // Create blend state for stacking layers (alpha over): later layers blend on
    // top of earlier ones using each eye texture's own alpha channel.
    D3D11_BLEND_DESC layerBlendDesc = {};
    layerBlendDesc.RenderTarget[0].BlendEnable = TRUE;
    layerBlendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    layerBlendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    layerBlendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    layerBlendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
    layerBlendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    layerBlendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    layerBlendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    hr = m_pDevice->CreateBlendState(&layerBlendDesc, &m_pLayerBlend);
    if (FAILED(hr)) {
        ENCODER_ERROR("Failed to create layer blend state! HRESULT: 0x%x", hr);
        vsBlob->Release();
        psBlob->Release();
        return false;
    }

    // Constant buffer carrying the per-layer source sub-rect (VRTextureBounds)
    // consumed by the SBS pixel shader.
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(float) * 8; // two float4
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = m_pDevice->CreateBuffer(&cbDesc, nullptr, &m_pBoundsCB);
    if (FAILED(hr)) {
        ENCODER_ERROR("Failed to create bounds constant buffer! HRESULT: 0x%x", hr);
        vsBlob->Release();
        psBlob->Release();
        return false;
    }

    // Create input layout (fullscreen triangle uses SV_VertexID, but need dummy layout)
    D3D11_INPUT_ELEMENT_DESC inputDesc = { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
    hr = m_pDevice->CreateInputLayout(&inputDesc, 1, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_pBlitInputLayout);
    vsBlob->Release();
    psBlob->Release();

    if (FAILED(hr)) {
        ENCODER_ERROR("Failed to create input layout! HRESULT: 0x%x", hr);
        return false;
    }

    // Create a tiny vertex buffer (needed for input layout binding)
    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = 12;
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    float dummyVertex[3] = { 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA vbData = {};
    vbData.pSysMem = dummyVertex;

    hr = m_pDevice->CreateBuffer(&vbDesc, &vbData, &m_pBlitVertexBuffer);
    if (FAILED(hr)) {
        ENCODER_ERROR("Failed to create vertex buffer! HRESULT: 0x%x", hr);
        return false;
    }

    // Create private staging textures for safe reading from shared textures
    // These are non-shared, private textures we copy to before shader conversion
    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = m_width / 2;  // Per-eye size for SBS
    stagingDesc.Height = m_height;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // Match our texture creation format
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_DEFAULT;
    stagingDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    hr = m_pDevice->CreateTexture2D(&stagingDesc, nullptr, &m_pLeftStaging);
    if (FAILED(hr)) {
        ENCODER_ERROR("Failed to create left staging texture! HRESULT: 0x%x", hr);
        return false;
    }

    hr = m_pDevice->CreateTexture2D(&stagingDesc, nullptr, &m_pRightStaging);
    if (FAILED(hr)) {
        ENCODER_ERROR("Failed to create right staging texture! HRESULT: 0x%x", hr);
        return false;
    }

    // Single staging for ConvertViaShader (full encoder width)
    stagingDesc.Width = m_width;
    hr = m_pDevice->CreateTexture2D(&stagingDesc, nullptr, &m_pSingleStaging);
    if (FAILED(hr)) {
        ENCODER_ERROR("Failed to create single staging texture! HRESULT: 0x%x", hr);
        return false;
    }

    ENCODER_LOG("Private staging textures created (%dx%d per eye, %dx%d single)",
                m_width / 2, m_height, m_width, m_height);

    m_shaderConversionReady = true;
    ENCODER_LOG("Shader-based texture conversion initialized successfully!");
    return true;
}

void VideoEncoder::CleanupShaderConversion()
{
    ENCODER_LOG("Cleaning up shader conversion resources...");

    if (m_pConversionRT) { m_pConversionRT->Release(); m_pConversionRT = nullptr; }
    if (m_pConversionRTV) { m_pConversionRTV->Release(); m_pConversionRTV = nullptr; }
    if (m_pBlitVS) { m_pBlitVS->Release(); m_pBlitVS = nullptr; }
    if (m_pBlitPS) { m_pBlitPS->Release(); m_pBlitPS = nullptr; }
    if (m_pSBSPS) { m_pSBSPS->Release(); m_pSBSPS = nullptr; }
    if (m_pBlitSampler) { m_pBlitSampler->Release(); m_pBlitSampler = nullptr; }
    if (m_pBlitBlend) { m_pBlitBlend->Release(); m_pBlitBlend = nullptr; }
    if (m_pLayerBlend) { m_pLayerBlend->Release(); m_pLayerBlend = nullptr; }
    if (m_pBoundsCB) { m_pBoundsCB->Release(); m_pBoundsCB = nullptr; }
    if (m_pBlitInputLayout) { m_pBlitInputLayout->Release(); m_pBlitInputLayout = nullptr; }
    if (m_pBlitVertexBuffer) { m_pBlitVertexBuffer->Release(); m_pBlitVertexBuffer = nullptr; }
    if (m_pLeftStaging) { m_pLeftStaging->Release(); m_pLeftStaging = nullptr; }
    if (m_pRightStaging) { m_pRightStaging->Release(); m_pRightStaging = nullptr; }
    if (m_pSingleStaging) { m_pSingleStaging->Release(); m_pSingleStaging = nullptr; }

    m_shaderConversionReady = false;
    ENCODER_LOG("Shader conversion resources cleaned up.");
}

void VideoEncoder::SetupBlitPipeline(ID3D11ShaderResourceView* pSRV)
{
    // Set full-screen render target
    float clearColor[4] = { 0, 0, 0, 1 };
    m_pContext->ClearRenderTargetView(m_pConversionRTV, clearColor);
    m_pContext->OMSetRenderTargets(1, &m_pConversionRTV, nullptr);

    // Set viewport to full conversion RT
    D3D11_VIEWPORT vp = {};
    vp.Width = (float)m_width;
    vp.Height = (float)m_height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_pContext->RSSetViewports(1, &vp);

    // Set shaders
    m_pContext->VSSetShader(m_pBlitVS, nullptr, 0);
    m_pContext->PSSetShader(m_pBlitPS, nullptr, 0);

    // Bind source texture and sampler
    m_pContext->PSSetShaderResources(0, 1, &pSRV);
    m_pContext->PSSetSamplers(0, 1, &m_pBlitSampler);

    // Set blend state
    float blendFactor[4] = { 0, 0, 0, 0 };
    m_pContext->OMSetBlendState(m_pBlitBlend, blendFactor, 0xFFFFFFFF);

    // Set input layout and vertex buffer
    m_pContext->IASetInputLayout(m_pBlitInputLayout);
    UINT stride = 12;
    UINT offset = 0;
    m_pContext->IASetVertexBuffers(0, 1, &m_pBlitVertexBuffer, &stride, &offset);
    m_pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

bool VideoEncoder::ReadBackBegin()
{
    // D3D11 only: Copy conversion RT to staging texture and map it for CPU read.
    // Caller must call sws_scale on mapped data, then call ReadBackEnd().
    m_pContext->CopySubresourceRegion(m_pStagingTexture, 0, 0, 0, 0, m_pConversionRT, 0, nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_pContext->Map(m_pStagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        ENCODER_ERROR("Failed to map staging texture! HRESULT: 0x%x", hr);
        return false;
    }

    m_mappedRowPitch = mapped.RowPitch;
    m_mappedData = mapped.pData;
    return true;
}

bool VideoEncoder::ReadBackEnd()
{
    // D3D11 only: unmap the staging texture after CPU read is done.
    m_pContext->Unmap(m_pStagingTexture, 0);
    m_mappedData = nullptr;
    return true;
}

bool VideoEncoder::ReadbackToBuffer()
{
    // D3D11: CopySubresourceRegion + Map + memcpy to CPU buffer + Unmap.
    // All D3D11 calls happen here so the caller never needs a mutex.
    // The CPU buffer is consumed later by SwsConvert.
    if (!m_initialized) {
        ENCODER_ERROR("ReadbackToBuffer called without valid encoder!");
        return false;
    }

    m_pContext->CopySubresourceRegion(m_pStagingTexture, 0, 0, 0, 0, m_pConversionRT, 0, nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_pContext->Map(m_pStagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        ENCODER_ERROR("Failed to map staging texture in ReadbackToBuffer! HRESULT: 0x%x", hr);
        return false;
    }

    int bufSize = m_height * mapped.RowPitch;
    if (!m_pReadbackBuffer || m_readbackBufferSize < bufSize) {
        delete[] m_pReadbackBuffer;
        m_pReadbackBuffer = new uint8_t[bufSize];
        m_readbackBufferSize = bufSize;
    }
    m_readbackRowPitch = mapped.RowPitch;

    // Copy row by row (staging RowPitch may differ from our expected stride)
    for (int y = 0; y < m_height; y++) {
        memcpy(m_pReadbackBuffer + y * m_width * 4,
               (uint8_t*)mapped.pData + y * mapped.RowPitch,
               m_width * 4);
    }

    // DIAG (Test A): detect black frames. m_pConversionRT is B8G8R8A8_UNORM, so the
    // mapped BGRA data lets us sample luminance. A near-zero mean means the encoder
    // is about to stream a black frame (confirms the layer compositing produced black).
#ifdef DRIVER_DIAG
    {
        const uint8_t* p = (const uint8_t*)mapped.pData;
        uint64_t sum = 0;
        int n = 0;
        for (int y = 0; y < m_height; y += 8) {
            const uint8_t* row = p + (size_t)y * mapped.RowPitch;
            for (int x = 0; x < m_width; x += 8) {
                const uint8_t* px = row + x * 4;
                sum += (uint64_t)px[0] + px[1] + px[2]; // B + G + R
                n++;
            }
        }
        if (n > 0) {
            uint64_t mean = sum / (uint64_t)n;
            if (mean < 4) { // average channel < ~4/255
                ENCODER_LOG("[DIAG BLACK FRAME] meanRGB=%llu n=%d Lfmt=0x%x Rfmt=0x%x",
                    (unsigned long long)mean, n, m_lastLeftFmt, m_lastRightFmt);
            }
        }
    }
#endif

    m_pContext->Unmap(m_pStagingTexture, 0);
    return true;
}

bool VideoEncoder::ReadBackConversionRT()
{
    if (!ReadBackBegin()) return false;

    // CPU-only: convert BGRA8 -> NV12
    uint8_t* srcSlice[1] = { (uint8_t*)m_mappedData };
    int srcStride[1] = { static_cast<int>(m_mappedRowPitch) };

    uint8_t* dstSlice[2] = { m_pSoftwareFrameBuffer, m_pSoftwareFrameBuffer + m_width * m_height };
    int dstStride[2] = { m_width, m_width };

    int ret = sws_scale(m_pConvertContext, srcSlice, srcStride, 0, m_height,
                        dstSlice, dstStride);

    if (!ReadBackEnd()) return false;

    if (ret != m_height) {
        ENCODER_ERROR("swscale conversion failed! Returned %d, expected %d", ret, m_height);
        return false;
    }

    return true;
}

bool VideoEncoder::ConvertViaShader(ID3D11Texture2D* pSource)
{
    if (!m_shaderConversionReady) {
        ENCODER_ERROR("Shader conversion not ready!");
        return false;
    }

    D3D11_TEXTURE2D_DESC srcDesc;
    pSource->GetDesc(&srcDesc);

    // Create SRV directly from source texture
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = srcDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;

    ID3D11ShaderResourceView* pSRV = nullptr;
    HRESULT hr = m_pDevice->CreateShaderResourceView(pSource, &srvDesc, &pSRV);
    if (FAILED(hr)) {
        ENCODER_ERROR("Failed to create SRV for source texture! HRESULT: 0x%x", hr);
        return false;
    }

    // Save current render state
    ID3D11RenderTargetView* pOldRTV = nullptr;
    ID3D11DepthStencilView* pOldDSV = nullptr;
    m_pContext->OMGetRenderTargets(1, &pOldRTV, &pOldDSV);

    D3D11_VIEWPORT oldViewport;
    UINT numViewports = 1;
    m_pContext->RSGetViewports(&numViewports, &oldViewport);

    // Setup blit pipeline (sets RT, viewport, shaders, sampler, blend)
    SetupBlitPipeline(pSRV);

    // Draw fullscreen triangle (3 vertices via SV_VertexID)
    m_pContext->Draw(3, 0);

    // Unbind SRV
    ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
    m_pContext->PSSetShaderResources(0, 1, nullSRV);

    // Restore render state
    m_pContext->OMSetRenderTargets(1, &pOldRTV, pOldDSV);
    m_pContext->RSSetViewports(1, &oldViewport);

    if (pOldRTV) pOldRTV->Release();
    if (pOldDSV) pOldDSV->Release();
    pSRV->Release();

    // Read back and convert to NV12
    return ReadBackConversionRT();
}

bool VideoEncoder::ComposeSBSLayer(ID3D11Texture2D* pLeft, ID3D11Texture2D* pRight,
                                     const LayerBounds& leftBounds, const LayerBounds& rightBounds,
                                     ID3D11BlendState* blendState)
{
    D3D11_TEXTURE2D_DESC leftDesc, rightDesc;
    pLeft->GetDesc(&leftDesc);
    pRight->GetDesc(&rightDesc);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;

    srvDesc.Format = leftDesc.Format;
    ID3D11ShaderResourceView* pLeftSRV = nullptr;
    HRESULT hr = m_pDevice->CreateShaderResourceView(pLeft, &srvDesc, &pLeftSRV);
    if (FAILED(hr)) {
        ENCODER_ERROR("Failed to create left eye SRV! HRESULT: 0x%x", hr);
        return false;
    }
    srvDesc.Format = rightDesc.Format;
    ID3D11ShaderResourceView* pRightSRV = nullptr;
    hr = m_pDevice->CreateShaderResourceView(pRight, &srvDesc, &pRightSRV);
    if (FAILED(hr)) {
        ENCODER_ERROR("Failed to create right eye SRV! HRESULT: 0x%x", hr);
        pLeftSRV->Release();
        return false;
    }

    // Upload this layer's source sub-rect (VRTextureBounds) to the bounds CB,
    // consumed by the SBS pixel shader as (offsetU, offsetV, scaleU, scaleV).
    float rects[8] = {
        leftBounds.uMin, leftBounds.vMin, leftBounds.uMax - leftBounds.uMin, leftBounds.vMax - leftBounds.vMin,
        rightBounds.uMin, rightBounds.vMin, rightBounds.uMax - rightBounds.uMin, rightBounds.vMax - rightBounds.vMin
    };
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = m_pContext->Map(m_pBoundsCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        memcpy(mapped.pData, rects, sizeof(rects));
        m_pContext->Unmap(m_pBoundsCB, 0);
    }

    ID3D11ShaderResourceView* srvs[2] = { pLeftSRV, pRightSRV };
    m_pContext->PSSetShaderResources(0, 2, srvs);
    m_pContext->PSSetConstantBuffers(0, 1, &m_pBoundsCB);

    float blendFactor[4] = { 0, 0, 0, 0 };
    m_pContext->OMSetBlendState(blendState ? blendState : m_pLayerBlend, blendFactor, 0xFFFFFFFF);

    m_pContext->Draw(3, 0);

    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
    m_pContext->PSSetShaderResources(0, 2, nullSRVs);

    pLeftSRV->Release();
    pRightSRV->Release();
    return true;
}

bool VideoEncoder::ComposeSBSGPU(const std::vector<ID3D11Texture2D*>& lefts,
                                 const std::vector<ID3D11Texture2D*>& rights,
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

    // DIAG (Test A): record eye formats + sample log every ~120 composes (H6 check)
    D3D11_TEXTURE2D_DESC dL = {}, dR = {};
    lefts.front()->GetDesc(&dL);
    rights.front()->GetDesc(&dR);
    m_lastLeftFmt = (uint32_t)dL.Format;
    m_lastRightFmt = (uint32_t)dR.Format;
#ifdef DRIVER_DIAG
    {
        static int s_composeDiag = 0;
        if (++s_composeDiag % 120 == 0) {
            ENCODER_LOG("[DIAG compose] layers=%zu L fmt=0x%x (%ux%u) R fmt=0x%x (%ux%u)",
                lefts.size(),
                (uint32_t)dL.Format, dL.Width, dL.Height,
                (uint32_t)dR.Format, dR.Width, dR.Height);
        }
    }
#endif

    // Save current render state
    ID3D11RenderTargetView* pOldRTV = nullptr;
    ID3D11DepthStencilView* pOldDSV = nullptr;
    m_pContext->OMGetRenderTargets(1, &pOldRTV, &pOldDSV);

    D3D11_VIEWPORT oldViewport;
    UINT numViewports = 1;
    m_pContext->RSGetViewports(&numViewports, &oldViewport);

    // Clear to transparent black and set the SBS conversion RT once.
    float clearColor[4] = { 0, 0, 0, 0 };
    m_pContext->ClearRenderTargetView(m_pConversionRTV, clearColor);
    m_pContext->OMSetRenderTargets(1, &m_pConversionRTV, nullptr);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)m_width;
    vp.Height = (float)m_height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_pContext->RSSetViewports(1, &vp);

    m_pContext->VSSetShader(m_pBlitVS, nullptr, 0);
    m_pContext->PSSetShader(m_pSBSPS, nullptr, 0);
    m_pContext->PSSetSamplers(0, 1, &m_pBlitSampler);
    m_pContext->IASetInputLayout(m_pBlitInputLayout);
    UINT stride = 12;
    UINT offset = 0;
    m_pContext->IASetVertexBuffers(0, 1, &m_pBlitVertexBuffer, &stride, &offset);
    m_pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Composite every layer in submission order (painter's algorithm): the first
    // (scene) is drawn opaque (alpha ignored) so it always fills the frame, later
    // layers (overlays) alpha-blend on top. Drawing the base opaque also avoids the
    // eye textures' alpha channel (often 0) making the scene invisible/black.
    size_t n = lefts.size();
    for (size_t i = 0; i < n; i++) {
        const LayerBounds& lbL = (i < leftBounds.size()) ? leftBounds[i] : LayerBounds{};
        const LayerBounds& lbR = (i < rightBounds.size()) ? rightBounds[i] : LayerBounds{};
        ID3D11BlendState* blend = (i == 0) ? m_pBlitBlend : m_pLayerBlend;
        if (!ComposeSBSLayer(lefts[i], rights[i], lbL, lbR, blend)) {
            ENCODER_ERROR("Failed to compose SBS layer %zu!", i);
        }
    }

    // Restore render state
    m_pContext->OMSetRenderTargets(1, &pOldRTV, pOldDSV);
    m_pContext->RSSetViewports(1, &oldViewport);

    if (pOldRTV) pOldRTV->Release();
    if (pOldDSV) pOldDSV->Release();

    return true;
}

bool VideoEncoder::FinishFrame(int64_t pts)
{
    if (!m_initialized) {
        ENCODER_ERROR("FinishFrame called without valid encoder!");
        return false;
    }

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    if (m_lastCallUs != 0) {
        int64_t intervalUs = ((t0.QuadPart - m_lastCallUs) * 1000000) / m_perfFreq.QuadPart;
        m_intervalSumUs += intervalUs;
        if (intervalUs > m_intervalMaxUs) m_intervalMaxUs = intervalUs;
        m_intervalCount++;
    }
    m_lastCallUs = t0.QuadPart;

    if (!ReadBackConversionRT()) {
        ENCODER_ERROR("Failed to read back conversion RT!");
        return false;
    }

    // Detect duplicate/stale capture (image-in-image symptom: same readback twice)
    uint32_t hash = ComputeFrameHash();
    if (m_lastFrameHash != 0 && hash == m_lastFrameHash) m_dupCount++;
    m_lastFrameHash = hash;
    m_summaryFrames++;

    m_pFrame->pts = pts;
    m_hasValidFrame = true;

    if (!SendFrameToEncoder()) {
        ENCODER_ERROR("Failed to send SBS frame to encoder!");
        return false;
    }

    if (!ReceiveEncodedPackets()) {
        ENCODER_ERROR("Failed to receive SBS encoded packets!");
        return false;
    }

    QueryPerformanceCounter(&t1);
    int64_t elapsedUs = ((t1.QuadPart - t0.QuadPart) * 1000000) / m_perfFreq.QuadPart;
    m_encSumUs += elapsedUs;
    if (elapsedUs > m_encMaxUs) m_encMaxUs = elapsedUs;
    m_encCount++;

    if (elapsedUs > 40000) {
        ENCODER_LOG("[TELEMETRY] SLOW frame: %lldus encode (exceeds frame budget)", (long long)elapsedUs);
    }

    if (m_encCount > 0 && (m_encCount % m_summaryInterval == 0)) {
        LogTelemetrySummary();
    }

    return true;
}

bool VideoEncoder::SwsConvert()
{
    // CPU-only: convert BGRA→NV12 using the readback buffer (populated by ReadbackToBuffer).
    uint8_t* srcData = m_pReadbackBuffer;
    int srcPitch = m_width * 4;

    if (!srcData) {
        ENCODER_ERROR("SwsConvert called without readback data!");
        return false;
    }

    uint8_t* srcSlice[1] = { srcData };
    int srcStride[1] = { srcPitch };

    uint8_t* dstSlice[2] = { m_pSoftwareFrameBuffer, m_pSoftwareFrameBuffer + m_width * m_height };
    int dstStride[2] = { m_width, m_width };

    int ret = sws_scale(m_pConvertContext, srcSlice, srcStride, 0, m_height,
                        dstSlice, dstStride);

    if (ret != m_height) {
        ENCODER_ERROR("swscale conversion failed! Returned %d, expected %d", ret, m_height);
        return false;
    }

    return true;
}

bool VideoEncoder::FinishEncode(int64_t pts)
{
    if (!m_initialized) {
        ENCODER_ERROR("FinishEncode called without valid encoder!");
        return false;
    }

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    if (m_lastCallUs != 0) {
        int64_t intervalUs = ((t0.QuadPart - m_lastCallUs) * 1000000) / m_perfFreq.QuadPart;
        m_intervalSumUs += intervalUs;
        if (intervalUs > m_intervalMaxUs) m_intervalMaxUs = intervalUs;
        m_intervalCount++;
    }
    m_lastCallUs = t0.QuadPart;

    uint32_t hash = ComputeFrameHash();
    if (m_lastFrameHash != 0 && hash == m_lastFrameHash) m_dupCount++;
    m_lastFrameHash = hash;
    m_summaryFrames++;

    m_pFrame->pts = pts;
    m_hasValidFrame = true;

    if (!SendFrameToEncoder()) {
        ENCODER_ERROR("Failed to send SBS frame to encoder!");
        return false;
    }

    if (!ReceiveEncodedPackets()) {
        ENCODER_ERROR("Failed to receive SBS encoded packets!");
        return false;
    }

    QueryPerformanceCounter(&t1);
    int64_t elapsedUs = ((t1.QuadPart - t0.QuadPart) * 1000000) / m_perfFreq.QuadPart;
    m_encSumUs += elapsedUs;
    if (elapsedUs > m_encMaxUs) m_encMaxUs = elapsedUs;
    m_encCount++;

    if (elapsedUs > 40000) {
        ENCODER_LOG("[TELEMETRY] SLOW frame: %lldus encode (exceeds frame budget)", (long long)elapsedUs);
    }

    if (m_encCount > 0 && (m_encCount % m_summaryInterval == 0)) {
        LogTelemetrySummary();
    }

    return true;
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

bool VideoEncoder::ConvertTextureToFrame(ID3D11Texture2D* pTexture)
{
    D3D11_TEXTURE2D_DESC desc;
    pTexture->GetDesc(&desc);

    // BGRA8 at matching resolution -> direct copy + sws_scale
    if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM &&
        desc.Width == (UINT)m_width && desc.Height == (UINT)m_height) {
        m_pContext->CopySubresourceRegion(m_pStagingTexture, 0, 0, 0, 0, pTexture, 0, nullptr);

        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = m_pContext->Map(m_pStagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) return false;

        uint8_t* srcSlice[1] = { (uint8_t*)mapped.pData };
        int srcStride[1] = { static_cast<int>(mapped.RowPitch) };
        uint8_t* dstSlice[2] = { m_pSoftwareFrameBuffer, m_pSoftwareFrameBuffer + m_width * m_height };
        int dstStride[2] = { m_width, m_width };

        int ret = sws_scale(m_pConvertContext, srcSlice, srcStride, 0, m_height, dstSlice, dstStride);
        m_pContext->Unmap(m_pStagingTexture, 0);
        return (ret == m_height);
    }

    // R10G10B10A2 or any other format -> shader conversion (CopySubresourceRegion returns uniform values on AMD)
    return ConvertViaShader(pTexture);
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

        if (m_pBsfCtx) {
            // Convert AVCC (length-prefixed) -> Annex B (start codes).
            // av_bsf_send_packet() takes ownership of m_pPacket.
            if (av_bsf_send_packet(m_pBsfCtx, m_pPacket) == 0) {
                AVPacket* filtered = av_packet_alloc();
                while (av_bsf_receive_packet(m_pBsfCtx, filtered) >= 0) {
                    if (m_encodedPacketCallback) {
                        m_encodedPacketCallback(filtered->data, filtered->size,
                                                filtered->pts, keyframe);
                    }
                    av_packet_unref(filtered);
                }
                av_packet_free(&filtered);
            }
            // m_pPacket is consumed by the BSF; reset for next receive.
            av_packet_unref(m_pPacket);
            continue;
        }

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

uint32_t VideoEncoder::ComputeFrameHash()
{
    if (!m_pSoftwareFrameBuffer) return 0;
    const uint8_t* p = m_pSoftwareFrameBuffer;
    int total = m_width * m_height * 3 / 2; // valid NV12 region only
    uint32_t h = 2166136261u;
    for (int i = 0; i < total; i += 1024) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

void VideoEncoder::LogTelemetrySummary()
{
    int64_t frames = m_encCount > 0 ? m_encCount : 1;
    int64_t avgEnc = m_encSumUs / frames;
    int64_t avgInt = m_intervalCount > 0 ? m_intervalSumUs / m_intervalCount : 0;
    double dupRate = m_summaryFrames > 0 ? (100.0 * m_dupCount / m_summaryFrames) : 0.0;
    ENCODER_LOG("[TELEMETRY] frames=%lld avgEncode=%lldus maxEncode=%lldus avgInterval=%lldus maxInterval=%lldus dups=%d dupRate=%.1f%%",
                (long long)m_encCount, (long long)avgEnc, (long long)m_encMaxUs,
                (long long)avgInt, (long long)m_intervalMaxUs,
                m_dupCount, dupRate);
    m_encSumUs = 0; m_encMaxUs = 0; m_encCount = 0;
    m_intervalSumUs = 0; m_intervalMaxUs = 0; m_intervalCount = 0;
    m_dupCount = 0; m_summaryFrames = 0;
}
