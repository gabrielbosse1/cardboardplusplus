#include "VideoEncoder.h"
#include "VideoEncoderFFmpeg.h"
#include "VideoEncoderLog.h"
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <cstring>
#pragma comment(lib, "d3dcompiler.lib")

// ---------------------------------------------------------------------------
// D3D11 texture conversion / SBS compositing pipeline.
//
// Turns the compositor's eye textures (any supported format, e.g.
// R10G10B10A2) into a single BGRA8 side-by-side frame on the encoding
// device, then reads it back to the CPU buffer consumed by SwsConvert()
// (in VideoEncoderFFmpeg.cpp).
//
// NOTE: DRIVER_DIAG is intentionally NOT defined in this translation unit.
// In the original code it was only defined in the HmdDriver.cpp TU, so the
// #ifdef DRIVER_DIAG blocks below compile out here exactly as they did in
// the baseline. Do not define the macro in a shared header or these blocks
// will quietly start logging.
// ---------------------------------------------------------------------------

namespace {

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

} // namespace

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