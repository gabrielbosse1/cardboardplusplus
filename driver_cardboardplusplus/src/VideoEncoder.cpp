#include "VideoEncoder.h"
#include "HmdDriver.h"
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <cstring>
#pragma comment(lib, "d3dcompiler.lib")

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

// SBS compose pixel shader - chooses left or right eye based on x coordinate
static const char* kSBSPSSource = R"(
Texture2D<float4> leftEye : register(t0);
Texture2D<float4> rightEye : register(t1);
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
    float4 c;
    if (input.uv.x < 0.5) {
        // Left eye: remap UV from [0, 0.5) to [0, 1]
        float2 leftUV = float2(input.uv.x * 2.0, input.uv.y);
        c = leftEye.Sample(samp, leftUV);
    } else {
        // Right eye: remap UV from [0.5, 1] to [0, 1]
        float2 rightUV = float2((input.uv.x - 0.5) * 2.0, input.uv.y);
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
    , m_hasValidFrame(false)
    , m_pConversionRT(nullptr)
    , m_pConversionRTV(nullptr)
    , m_pBlitVS(nullptr)
    , m_pBlitPS(nullptr)
    , m_pBlitSampler(nullptr)
    , m_pBlitBlend(nullptr)
    , m_pBlitInputLayout(nullptr)
    , m_pBlitVertexBuffer(nullptr)
    , m_shaderConversionReady(false)
    , m_pLeftStaging(nullptr)
    , m_pRightStaging(nullptr)
    , m_pSingleStaging(nullptr)
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

    m_initialized = false;
    ENCODER_LOG("VideoEncoder shutdown complete.");
}

bool VideoEncoder::InitializeFFmpeg()
{
    ENCODER_LOG("Initializing FFmpeg H264 encoder...");

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        ENCODER_ERROR("H264 encoder not found! Trying alternative encoders...");

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

    ENCODER_LOG("Using software encoding with NV12 pix_fmt...");
    m_pCodecContext->pix_fmt = AV_PIX_FMT_NV12;

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

    // Create blend state (no blending)
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

bool VideoEncoder::ReadBackConversionRT()
{
    // Copy conversion RT to staging texture
    m_pContext->CopySubresourceRegion(m_pStagingTexture, 0, 0, 0, 0, m_pConversionRT, 0, nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_pContext->Map(m_pStagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        ENCODER_ERROR("Failed to map staging texture! HRESULT: 0x%x", hr);
        return false;
    }

    // Convert BGRA8 -> NV12
    uint8_t* srcSlice[1] = { (uint8_t*)mapped.pData };
    int srcStride[1] = { static_cast<int>(mapped.RowPitch) };

    uint8_t* dstSlice[2] = { m_pSoftwareFrameBuffer, m_pSoftwareFrameBuffer + m_width * m_height };
    int dstStride[2] = { m_width, m_width };

    int ret = sws_scale(m_pConvertContext, srcSlice, srcStride, 0, m_height,
                        dstSlice, dstStride);

    m_pContext->Unmap(m_pStagingTexture, 0);

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

bool VideoEncoder::ComposeSBS(ID3D11Texture2D* pLeft, ID3D11Texture2D* pRight)
{
    if (!m_shaderConversionReady) {
        ENCODER_ERROR("Shader conversion not ready for SBS compositing!");
        return false;
    }

    D3D11_TEXTURE2D_DESC leftDesc, rightDesc;
    pLeft->GetDesc(&leftDesc);
    pRight->GetDesc(&rightDesc);

    // Create SRVs directly from source textures
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

    // Save current render state
    ID3D11RenderTargetView* pOldRTV = nullptr;
    ID3D11DepthStencilView* pOldDSV = nullptr;
    m_pContext->OMGetRenderTargets(1, &pOldRTV, &pOldDSV);

    D3D11_VIEWPORT oldViewport;
    UINT numViewports = 1;
    m_pContext->RSGetViewports(&numViewports, &oldViewport);

    // Clear and set conversion RT
    float clearColor[4] = { 0, 0, 0, 1 };
    m_pContext->ClearRenderTargetView(m_pConversionRTV, clearColor);
    m_pContext->OMSetRenderTargets(1, &m_pConversionRTV, nullptr);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)m_width;
    vp.Height = (float)m_height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_pContext->RSSetViewports(1, &vp);

    // Use SBS pixel shader with both eye textures
    m_pContext->VSSetShader(m_pBlitVS, nullptr, 0);
    m_pContext->PSSetShader(m_pSBSPS, nullptr, 0);

    // Bind both eye textures and sampler
    ID3D11ShaderResourceView* srvs[2] = { pLeftSRV, pRightSRV };
    m_pContext->PSSetShaderResources(0, 2, srvs);
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

    // Draw fullscreen triangle (SBS shader composites both eyes in one pass)
    m_pContext->Draw(3, 0);

    // Unbind SRVs
    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
    m_pContext->PSSetShaderResources(0, 2, nullSRVs);

    // Restore render state
    m_pContext->OMSetRenderTargets(1, &pOldRTV, pOldDSV);
    m_pContext->RSSetViewports(1, &oldViewport);

    if (pOldRTV) pOldRTV->Release();
    if (pOldDSV) pOldDSV->Release();
    pLeftSRV->Release();
    pRightSRV->Release();

    // Read back and convert to NV12
    return ReadBackConversionRT();
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
    if (!m_initialized) {
        ENCODER_ERROR("EncodeFrameSBS called without valid encoder!");
        return false;
    }

    if (!pLeft || !pRight) {
        ENCODER_ERROR("EncodeFrameSBS called with null eye textures!");
        return false;
    }

    if (!ComposeSBS(pLeft, pRight)) {
        ENCODER_ERROR("Failed to compose SBS frame!");
        return false;
    }

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

    return true;
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
