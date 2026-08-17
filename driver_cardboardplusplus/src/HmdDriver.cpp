#include "HmdDriver.h"
#include <dxgi.h>
#include <dxgi1_2.h>
#include <cstdarg>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <cstring>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using namespace vr;

static const int UDP_SERVER_PORT = 42069;
static const int UDP_DISCOVERY_PORT = 42070;

void DriverLog(const char* pFormat, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, pFormat);
    vsprintf_s(buffer, pFormat, args);
    strcat_s(buffer, "\n");
    vr::VRDriverLog()->Log(buffer);
    va_end(args);
}

// Virtual HMD driver implementation. Presents as a display device to SteamVR.
EVRInitError HmdDriver::Activate(uint32_t unObjectId)
{
    // When I wrote this code, only God and I understood it.
    // Now only God understands it.
    // If you're an atheist, good luck.
    // I even managed to somehow get the error "'cannot open file 'kernel32.lib'"
    driverId = unObjectId;
    m_currentSwapSetIndex = 0;
    m_encoderInitialized = false;
    m_encoderPts = 0;
    m_lastEncodedPid = 0;
    m_pVideoEncoder = nullptr;
    m_hasSubmit = false;
    m_udpSocket = INVALID_SOCKET;
    m_udpInitialized = false;
    m_discoverySocket = INVALID_SOCKET;
    m_discoveryInitialized = false;
    m_discoveryRunning = false;
    m_submitLayers[0] = { 0 };
    m_submitLayers[1] = { 0 };

    DriverLog("HmdDriver::Activate called");

    // Create a D3D11 device for rendering.
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &pD3D11Device,
        &featureLevel,
        &pD3D11DeviceContext
    );

    if (FAILED(hr)) {
        DriverLog("D3D11 device creation failed! HRESULT: 0x%x", hr);
        return VRInitError_Init_Internal;
    }

    DriverLog("D3D11 device initialized successfully");

    if (!InitializeVideoEncoder()) {
        DriverLog("WARNING: Video encoder initialization failed. Encoding will be disabled.");
    }

    if (!InitializeUDP()) {
        DriverLog("WARNING: UDP initialization failed. Frame transmission will be disabled.");
    }

    if (!InitializeDiscovery()) {
        DriverLog("WARNING: Discovery initialization failed. Phone auto-detection will be disabled.");
    }

    // Set HMD properties
    PropertyContainerHandle_t props = VRProperties()->TrackedDeviceToPropertyContainer(driverId);

    VRProperties()->SetStringProperty(props, Prop_ModelNumber_String, "CardboardPlusPlus");
    VRProperties()->SetStringProperty(props, Prop_RenderModelName_String, "CardboardPlusPlus");
    VRProperties()->SetStringProperty(props, Prop_SerialNumber_String, "CBPP_VIRTUAL_HMD_001");
    VRProperties()->SetInt32Property(props, Prop_DeviceClass_Int32, TrackedDeviceClass_HMD);
    VRProperties()->SetStringProperty(props, Prop_ManufacturerName_String, "CardboardPlusPlus");

    VRProperties()->SetStringProperty(props, Prop_TrackingSystemName_String, "cardboardplusplus");
    VRProperties()->SetFloatProperty(props, Prop_UserIpdMeters_Float, 0.064f);
    VRProperties()->SetFloatProperty(props, Prop_DisplayFrequency_Float, 60.0f);
    VRProperties()->SetFloatProperty(props, Prop_SecondsFromVsyncToPhotons_Float, 0.011f);
    VRProperties()->SetBoolProperty(props, Prop_ReportsTimeSinceVSync_Bool, true);

    VRProperties()->SetUint64Property(props, Prop_CurrentUniverseId_Uint64, 2);
    VRProperties()->SetFloatProperty(props, Prop_UserHeadToEyeDepthMeters_Float, 0.f);
    VRProperties()->SetBoolProperty(props, Prop_IsOnDesktop_Bool, false);
    VRProperties()->SetBoolProperty(props, Prop_DisplayDebugMode_Bool, false);
    VRProperties()->SetBoolProperty(props, Prop_HasDriverDirectModeComponent_Bool, true);

    DriverLog("HMD properties set: HasDriverDirectModeComponent=true, IsDisplayOnDesktop=false, DebugMode=false");

    // Eye-to-head transforms
    HmdMatrix34_t eyeToHeadLeft = { 0 };
    eyeToHeadLeft.m[0][0] = 1.0f;
    eyeToHeadLeft.m[1][1] = 1.0f;
    eyeToHeadLeft.m[2][2] = 1.0f;
    eyeToHeadLeft.m[0][3] = -0.032f; // left eye offset

    HmdMatrix34_t eyeToHeadRight = { 0 };
    eyeToHeadRight.m[0][0] = 1.0f;
    eyeToHeadRight.m[1][1] = 1.0f;
    eyeToHeadRight.m[2][2] = 1.0f;
    eyeToHeadRight.m[0][3] = 0.032f; // right eye offset

    VRServerDriverHost()->SetDisplayEyeToHead(driverId, eyeToHeadLeft, eyeToHeadRight);

    return VRInitError_None;
}

void HmdDriver::Deactivate()
{
	// Clean up resources and reset state.
    DriverLog("HmdDriver::Deactivate called");

    ShutdownDiscovery();
    ShutdownUDP();
    ShutdownVideoEncoder();
    DestroyAllSwapTextureSets(0);

    if (pD3D11DeviceContext) {
        pD3D11DeviceContext->Release();
        pD3D11DeviceContext = nullptr;
    }
    if (pD3D11Device) {
        pD3D11Device->Release();
        pD3D11Device = nullptr;
    }
    driverId = k_unTrackedDeviceIndexInvalid;
}

void HmdDriver::EnterStandby() {}

void* HmdDriver::GetComponent(const char* pchComponentNameAndVersion)
{
	// Return to SteamVR which interfaces we support. This is how SteamVR knows we have display and direct mode components.
    DriverLog("GetComponent called with: %s", pchComponentNameAndVersion);
    if (strcmp(pchComponentNameAndVersion, IVRDisplayComponent_Version) == 0)
    {
        return static_cast<IVRDisplayComponent*>(this);
    }
    if (strcmp(pchComponentNameAndVersion, IVRDriverDirectModeComponent_Version) == 0)
    {
        return static_cast<IVRDriverDirectModeComponent*>(this);
    }
    return NULL;
}

void HmdDriver::DebugRequest(const char* pchRequest, char* pchResponseBuffer, uint32_t unResponseBufferSize)
{
    if (unResponseBufferSize >= 1)
    {
        pchResponseBuffer[0] = 0;
    }
}

DriverPose_t HmdDriver::GetPose()
{
    DriverPose_t pose = { 0 };
    pose.poseIsValid = true;
    pose.result = TrackingResult_Running_OK;
    pose.deviceIsConnected = true;

    static auto startTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - startTime).count();

    float bobHeight = (float)(sin(elapsed * 2.0) * 0.02);
    float swayX = (float)(sin(elapsed * 1.5) * 0.01);

    HmdQuaternion_t quat;
    quat.w = 1.0;
    quat.x = 0.0;
    quat.y = 0.0;
    quat.z = 0.0;

    pose.qWorldFromDriverRotation = quat;
    pose.qDriverFromHeadRotation = quat;

    pose.vecPosition[0] = swayX;
    pose.vecPosition[1] = bobHeight;
    pose.vecPosition[2] = 0.0;

    return pose;
}

void HmdDriver::RunFrame()
{
    // Update the server with our current pose each frame so compositor knows this HMD is present.
    DriverPose_t pose = GetPose();
    VRServerDriverHost()->TrackedDevicePoseUpdated(driverId, pose, sizeof(DriverPose_t));
}

// IVRDisplayComponent implementations
void HmdDriver::GetWindowBounds( int32_t *pnX, int32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight )
{
    if (pnX) *pnX = 0;
    if (pnY) *pnY = 0;
    if (pnWidth) *pnWidth = 1920;
    if (pnHeight) *pnHeight = 1080;
}

bool HmdDriver::IsDisplayOnDesktop()
{
    return false;
}

bool HmdDriver::IsDisplayRealDisplay()
{
    return false;
}

void HmdDriver::GetRecommendedRenderTargetSize( uint32_t *pnWidth, uint32_t *pnHeight )
{
    if (pnWidth) *pnWidth = 1920 / 2; // single-eye recommended width
    if (pnHeight) *pnHeight = 1080; // single-eye recommended height
}

void HmdDriver::GetEyeOutputViewport( EVREye eEye, uint32_t *pnX, uint32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight )
{
    *pnY = 0;
    *pnWidth = 1920 / 2;
    *pnHeight = 1080;

    if (eEye == Eye_Left) {
        *pnX = 0;
    }
    else {
        *pnX = 1920 / 2;
    }
}

void HmdDriver::GetProjectionRaw( EVREye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom )
{
	// Return a simple symmetric projection for now. These values can be adjusted to change the FOV and aspect ratio.
    *pfLeft = -1.0;
    *pfRight = 1.0;
    *pfTop = -1.0;
    *pfBottom = 1.0;
}

DistortionCoordinates_t HmdDriver::ComputeDistortion( EVREye eEye, float fU, float fV )
{
	// No distortion at all, Cardboard SDK should handle this.
    DistortionCoordinates_t coordinates;
    coordinates.rfBlue[0] = fU;
    coordinates.rfBlue[1] = fV;
    coordinates.rfGreen[0] = fU;
    coordinates.rfGreen[1] = fV;
    coordinates.rfRed[0] = fU;
    coordinates.rfRed[1] = fV;
    return coordinates;
}

void HmdDriver::CreateSwapTextureSet(uint32_t unPid, const SwapTextureSetDesc_t* pSwapTextureSetDesc, SwapTextureSet_t* pOutSwapTextureSet)
{
	// Create a shared texture that the application can render into.
    DriverLog("CreateSwapTextureSet called: width=%d, height=%d, format=%d, samples=%d",
        pSwapTextureSetDesc->nWidth, pSwapTextureSetDesc->nHeight, pSwapTextureSetDesc->nFormat, pSwapTextureSetDesc->nSampleCount);

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = pSwapTextureSetDesc->nWidth;
    desc.Height = pSwapTextureSetDesc->nHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = (DXGI_FORMAT)pSwapTextureSetDesc->nFormat;  // Use SteamVR's exact format
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;  // No keyed mutex

    DriverLog("Creating texture: %dx%d format=%d flags=SHARED", desc.Width, desc.Height, desc.Format);

    ID3D11Texture2D* pTexture = nullptr;
    HRESULT hr = pD3D11Device->CreateTexture2D(&desc, nullptr, &pTexture);

    if (FAILED(hr)) {
        DriverLog("Failed to create texture! HRESULT: 0x%x", hr);
        return;
    }

    IDXGIResource* pDXGIResource = nullptr;
    hr = pTexture->QueryInterface(__uuidof(IDXGIResource), (void**)&pDXGIResource);
    if (FAILED(hr)) {
        DriverLog("Failed to get DXGI resource! HRESULT: 0x%x", hr);
        pTexture->Release();
        return;
    }

    HANDLE hSharedHandle = nullptr;
    hr = pDXGIResource->GetSharedHandle(&hSharedHandle);
    pDXGIResource->Release();

    if (FAILED(hr)) {
        DriverLog("Failed to get shared handle! HRESULT: 0x%x", hr);
        pTexture->Release();
        return;
    }

    DriverLog("Created texture with shared handle: %llu", (uint64_t)hSharedHandle);

    // Return the shared handle for all 3 buffers (triple buffered)
    pOutSwapTextureSet->rSharedTextureHandles[0] = (vr::SharedTextureHandle_t)hSharedHandle;
    pOutSwapTextureSet->rSharedTextureHandles[1] = (vr::SharedTextureHandle_t)hSharedHandle;
    pOutSwapTextureSet->rSharedTextureHandles[2] = (vr::SharedTextureHandle_t)hSharedHandle;
    pOutSwapTextureSet->unTextureFlags = 0;

    // Store for later lookup in SubmitLayer
    SwapTextureSet sts;
    sts.pTexture = pTexture;
    sts.hSharedHandle = hSharedHandle;
    sts.pKeyedMutex = nullptr;

    auto it = m_swapTextureSets.find(unPid);
    if (it == m_swapTextureSets.end()) {
        std::vector<SwapTextureSet> vec;
        vec.push_back(sts);
        m_swapTextureSets[unPid] = vec;
    } else {
        it->second.push_back(sts);
    }

    // Map handle -> texture for quick lookup in SubmitLayer
    m_textureHandleMap[(vr::SharedTextureHandle_t)hSharedHandle] = pTexture;
}

void HmdDriver::DestroySwapTextureSet(vr::SharedTextureHandle_t sharedTextureHandle)
{
	// Find the texture set with the given shared handle and release it.
    DriverLog("DestroySwapTextureSet called: handle=%llu", (uint64_t)sharedTextureHandle);

    HANDLE h = (HANDLE)sharedTextureHandle;

    // Remove from handle maps
    m_textureHandleMap.erase(sharedTextureHandle);
    m_mutexHandleMap.erase(sharedTextureHandle);

    for (auto& pair : m_swapTextureSets) {
        for (auto it = pair.second.begin(); it != pair.second.end(); ++it) {
            if (it->hSharedHandle == h) {
                if (it->pKeyedMutex) {
                    it->pKeyedMutex->Release();
                }
                if (it->pTexture) {
                    it->pTexture->Release();
                }
                pair.second.erase(it);
                return;
            }
        }
    }
}

void HmdDriver::DestroyAllSwapTextureSets(uint32_t unPid)
{
	// Release all texture sets associated with the given process ID.
    DriverLog("DestroyAllSwapTextureSets called for pid=%d", unPid);

    auto it = m_swapTextureSets.find(unPid);
    if (it != m_swapTextureSets.end()) {
        for (auto& sts : it->second) {
            // Remove from handle maps
            m_textureHandleMap.erase((vr::SharedTextureHandle_t)sts.hSharedHandle);
            m_mutexHandleMap.erase((vr::SharedTextureHandle_t)sts.hSharedHandle);
            if (sts.pKeyedMutex) {
                sts.pKeyedMutex->Release();
            }
            if (sts.pTexture) {
                sts.pTexture->Release();
            }
        }
        m_swapTextureSets.erase(it);
    }
}

void HmdDriver::GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t sharedTextureHandles[2], uint32_t(*pIndices)[2])
{
	// We just return index 0 for both eyes.
    DriverLog("GetNextSwapTextureSetIndex called");
    (*pIndices)[0] = 0;
    (*pIndices)[1] = 0;
}

void HmdDriver::SubmitLayer(const SubmitLayerPerEye_t(&perEye)[2])
{
	// This is where the application submits the textures it rendered for each eye.
    DriverLog("SubmitLayer called - left: %llu, right: %llu",
        (uint64_t)perEye[0].hTexture, (uint64_t)perEye[1].hTexture);

    // Store handles - encode only on Present (after all SubmitLayers for this frame)
    m_submitLayers[0].hTexture = perEye[0].hTexture;
    m_submitLayers[1].hTexture = perEye[1].hTexture;
    m_hasSubmit = true;
}

void HmdDriver::Present(vr::SharedTextureHandle_t syncTexture)
{
    // Count every Present SteamVR issues (compositor rate), independent of whether
    // we actually encode, so we can see if the stream is Present-bound or encode-bound.
    m_presentCount++;
    {
        long long nowNs = std::chrono::steady_clock::now().time_since_epoch().count();
        if (m_lastPresentLogNs == 0) m_lastPresentLogNs = nowNs;
        if (nowNs - m_lastPresentLogNs >= 1'000'000'000LL) {
            double presentFps = m_presentCount * 1e9 / (nowNs - m_lastPresentLogNs);
            DriverLog("Present rate: %.1f fps (count=%d)", presentFps, m_presentCount);
            m_presentCount = 0;
            m_lastPresentLogNs = nowNs;
        }
    }

    DriverLog("Present called! syncTexture=%llu, hasSubmit=%s",
        (uint64_t)syncTexture, m_hasSubmit ? "yes" : "no");

    if (!m_hasSubmit || !m_encoderInitialized || !m_pVideoEncoder) {
        return;
    }

    // Use stored texture pointers from the map (set up in CreateSwapTextureSet)
    ID3D11Texture2D* pLeftTex = nullptr;
    ID3D11Texture2D* pRightTex = nullptr;

    auto itL = m_textureHandleMap.find(m_submitLayers[0].hTexture);
    if (itL != m_textureHandleMap.end()) pLeftTex = itL->second;

    auto itR = m_textureHandleMap.find(m_submitLayers[1].hTexture);
    if (itR != m_textureHandleMap.end()) pRightTex = itR->second;

    DriverLog("Map lookup: left=%llu->%s, right=%llu->%s, map_size=%zu",
        (uint64_t)m_submitLayers[0].hTexture, pLeftTex ? "FOUND" : "NOT FOUND",
        (uint64_t)m_submitLayers[1].hTexture, pRightTex ? "FOUND" : "NOT FOUND",
        m_textureHandleMap.size());

    // Flush GPU pipeline to ensure we see vrcompositor's writes
    pD3D11DeviceContext->Flush();

    // Encode SBS - no mutex, no OpenSharedResource, just read directly
    if (pLeftTex && pRightTex) {
        std::lock_guard<std::mutex> lock(m_encoderMutex);
        if (!m_encoderInitialized || !m_pVideoEncoder) return;
        DriverLog("Encoding SBS: left=%llu right=%llu pts=%lld",
            (uint64_t)m_submitLayers[0].hTexture, (uint64_t)m_submitLayers[1].hTexture, m_encoderPts);
        m_pVideoEncoder->EncodeFrameSBS(pLeftTex, pRightTex, m_encoderPts);
        m_encoderPts++;
    } else if (pLeftTex) {
        std::lock_guard<std::mutex> lock(m_encoderMutex);
        if (!m_encoderInitialized || !m_pVideoEncoder) return;
        m_pVideoEncoder->EncodeFrame(pLeftTex, m_encoderPts);
        m_encoderPts++;
    }

    m_hasSubmit = false;
}

void HmdDriver::PostPresent()
{
	// This is called after Present returns, allowing the driver to take more time until vsync after they've successfully acquired the sync texture in Present. We can use this to do any additional work needed before the next frame.
    DriverLog("PostPresent called");
}

void HmdDriver::GetFrameTiming(DriverDirectMode_FrameTiming* pFrameTiming)
{
	// This is called to get additional frame timing stats from driver. Can be used to get the current framerate to optimize the encoder settings in real-time.
    DriverLog("GetFrameTiming called");
}

bool HmdDriver::InitializeVideoEncoder()
{
    DriverLog("========================================");
    DriverLog("Initializing Video Encoder...");
    DriverLog("========================================");

    if (!pD3D11Device || !pD3D11DeviceContext) {
        DriverLog("Cannot initialize encoder: D3D device not available!");
        return false;
    }

    m_pVideoEncoder = new VideoEncoder();
    if (!m_pVideoEncoder) {
        DriverLog("Failed to allocate VideoEncoder!");
        return false;
    }

    // SBS resolution: 1440x1620 per eye -> 2880x1620 (defaults, clamped to decoder cap)
    m_encoderW = 2880;
    m_encoderH = 1620;
    m_encoderFps = 60;
    m_encoderBitrate = 20000000;
    m_encoderUseGpu = false;
    ClampEncoderToCap();

    int width = m_encoderW;
    int height = m_encoderH;
    int fps = m_encoderFps;
    int bitrate = m_encoderBitrate;
    bool useGpuEncoding = m_encoderUseGpu;

    DriverLog("Encoder configuration:");
    DriverLog("  Resolution: %dx%d (SBS: %dx%d per eye)", width, height, width / 2, height);
    DriverLog("  FPS: %d", fps);
    DriverLog("  Bitrate: %d bps (%d kbps)", bitrate, bitrate / 1000);
    DriverLog("  GPU Encoding: %s", useGpuEncoding ? "YES" : "NO");

    m_pVideoEncoder->SetEncodedPacketCallback([this](uint8_t* data, int size, int64_t pts, bool keyframe) {
        OnEncodedPacket(data, size, pts, keyframe);
    });

    if (!m_pVideoEncoder->Initialize(pD3D11Device, pD3D11DeviceContext, width, height, fps, bitrate, useGpuEncoding)) {
        DriverLog("VideoEncoder::Initialize failed!");
        delete m_pVideoEncoder;
        m_pVideoEncoder = nullptr;
        return false;
    }

    m_encoderInitialized = true;
    m_encoderPts = 0;
    DriverLog("Video Encoder initialized successfully!");
    return true;
}

void HmdDriver::ShutdownVideoEncoder()
{
    DriverLog("Shutting down Video Encoder...");

    if (m_pVideoEncoder) {
        m_pVideoEncoder->Shutdown();
        delete m_pVideoEncoder;
        m_pVideoEncoder = nullptr;
    }

    m_encoderInitialized = false;
    DriverLog("Video Encoder shutdown complete.");
}

void HmdDriver::ClampEncoderToCap()
{
    if (m_pendingCapW <= 0 || m_pendingCapH <= 0) {
        return;
    }

    double scale = 1.0;
    double sx = (double)m_pendingCapW / (double)m_encoderW;
    double sy = (double)m_pendingCapH / (double)m_encoderH;
    if (sx < scale) scale = sx;
    if (sy < scale) scale = sy;

    if (scale >= 0.999) {
        return;
    }

    int newW = (int)(m_encoderW * scale);
    int newH = (int)(m_encoderH * scale);
    // Align down to 16 (H.264 macroblock requirement)
    newW = (newW / 16) * 16;
    newH = (newH / 16) * 16;

    if (newW > 0 && newH > 0) {
        DriverLog("Clamping encoder to hardware cap %dx%d: %dx%d -> %dx%d",
                  m_pendingCapW, m_pendingCapH, m_encoderW, m_encoderH, newW, newH);
        m_encoderW = newW;
        m_encoderH = newH;
    }
}

bool HmdDriver::ApplyHardwareCap(int capW, int capH)
{
    std::lock_guard<std::mutex> lock(m_encoderMutex);

    m_pendingCapW = capW;
    m_pendingCapH = capH;

    if (!m_encoderInitialized || !m_pVideoEncoder) {
        // Encoder not up yet; cap will be applied at initialization time.
        DriverLog("Hardware cap %dx%d received (encoder not ready, applied on init)", capW, capH);
        return true;
    }

    int oldW = m_encoderW;
    int oldH = m_encoderH;
    ClampEncoderToCap();

    if (m_encoderW == oldW && m_encoderH == oldH) {
        DriverLog("Hardware cap %dx%d >= current encoder %dx%d, no change", capW, capH, oldW, oldH);
        return false;
    }

    DriverLog("Re-initializing encoder under hardware cap %dx%d: %dx%d -> %dx%d",
              capW, capH, oldW, oldH, m_encoderW, m_encoderH);

    m_pVideoEncoder->Shutdown();
    delete m_pVideoEncoder;
    m_pVideoEncoder = nullptr;
    m_encoderInitialized = false;

    m_pVideoEncoder = new VideoEncoder();
    if (!m_pVideoEncoder) {
        DriverLog("Failed to allocate VideoEncoder during cap re-init!");
        return false;
    }

    m_pVideoEncoder->SetEncodedPacketCallback([this](uint8_t* data, int size, int64_t pts, bool keyframe) {
        OnEncodedPacket(data, size, pts, keyframe);
    });

    if (!m_pVideoEncoder->Initialize(pD3D11Device, pD3D11DeviceContext,
                                     m_encoderW, m_encoderH, m_encoderFps, m_encoderBitrate, m_encoderUseGpu)) {
        DriverLog("VideoEncoder re-init failed under cap!");
        delete m_pVideoEncoder;
        m_pVideoEncoder = nullptr;
        return false;
    }

    m_encoderInitialized = true;
    m_encoderPts = 0;
    DriverLog("Encoder re-initialized at %dx%d under hardware cap", m_encoderW, m_encoderH);
    return true;
}

void HmdDriver::OnEncodedPacket(uint8_t* data, int size, int64_t pts, bool keyframe)
{
    if (keyframe && size >= 8) {
        DriverLog("[Encoded] size=%d, pts=%lld, keyframe=YES, first16=%02X %02X %02X %02X %02X %02X %02X %02X",
                  size, pts,
                  data[0], data[1], data[2], data[3],
                  data[4], data[5], data[6], data[7]);
    } else {
        DriverLog("[Encoded] size=%d, pts=%lld, keyframe=%s, data[0]=0x%02X",
                  size, pts, keyframe ? "YES" : "NO", data[0]);
    }

    if (!m_udpInitialized || m_udpSocket == INVALID_SOCKET || size <= 0) {
        return;
    }

    // libx264 keyframes: SPS and PPS have NAL start codes, but IDR data follows
    // without one. Insert IDR start code after PPS for valid H264 stream.
    static const uint8_t idr_prefix[] = { 0x00, 0x00, 0x00, 0x01, 0x65 };
    bool needs_fix = false;
    int pps_data_end = -1;

    if (keyframe && size > 10) {
        // Find PPS NAL (type 8) and its data end
        for (int i = 0; i < size - 5; i++) {
            if (data[i] == 0x00 && data[i+1] == 0x00 && data[i+2] == 0x00 &&
                data[i+3] == 0x01 && (data[i+4] & 0x1F) == 8) {
                // Found PPS start. Find next NAL start code or end of small PPS
                for (int j = i + 5; j < size - 3; j++) {
                    if (data[j] == 0x00 && data[j+1] == 0x00 &&
                        ((data[j+2] == 0x01) || (data[j+2] == 0x00 && data[j+3] == 0x01))) {
                        pps_data_end = j;
                        break;
                    }
                }
                if (pps_data_end < 0) pps_data_end = i + 5 + 4; // assume short PPS
                // Check if IDR start code already present. libx264 emits a 3-byte
                // 00 00 01 65 start code after PPS, so accept both 3- and 4-byte forms.
                bool idr_sc_present = false;
                if (pps_data_end + 3 < size &&
                    data[pps_data_end] == 0x00 && data[pps_data_end+1] == 0x00 &&
                    data[pps_data_end+2] == 0x01 &&
                    (data[pps_data_end+3] & 0x1F) == 5) {
                    idr_sc_present = true; // 3-byte start code: 00 00 01 65
                } else if (pps_data_end + 4 < size &&
                    data[pps_data_end] == 0x00 && data[pps_data_end+1] == 0x00 &&
                    data[pps_data_end+2] == 0x00 && data[pps_data_end+3] == 0x01 &&
                    (data[pps_data_end+4] & 0x1F) == 5) {
                    idr_sc_present = true; // 4-byte start code: 00 00 00 01 65
                }
                if (idr_sc_present) {
                    break; // IDR start code already present, no fix needed
                }
                needs_fix = true;
                break;
            }
        }
    }

    if (needs_fix && pps_data_end > 0) {
        // Build fixed buffer: [up to PPS end] + [IDR start code] + [remaining IDR data]
        int fixed_size = size + 5; // extra 5 bytes for NAL start + IDR header
        uint8_t* fixed = (uint8_t*)malloc(fixed_size);
        memcpy(fixed, data, pps_data_end);
        memcpy(fixed + pps_data_end, idr_prefix, 5);
        memcpy(fixed + pps_data_end + 5, data + pps_data_end, size - pps_data_end);

        // Frame the fixed keyframe with a 4-byte big-endian length prefix so
        // the receiver can reconstruct the exact AVPacket (one full frame).
        int framed_size = fixed_size + 4;
        uint8_t* framed = (uint8_t*)malloc(framed_size);
        if (framed) {
            framed[0] = (uint8_t)((fixed_size >> 24) & 0xFF);
            framed[1] = (uint8_t)((fixed_size >> 16) & 0xFF);
            framed[2] = (uint8_t)((fixed_size >> 8) & 0xFF);
            framed[3] = (uint8_t)(fixed_size & 0xFF);
            memcpy(framed + 4, fixed, fixed_size);

            int offset = 0;
            while (offset < framed_size) {
                int cs = (framed_size - offset > 60000) ? 60000 : (framed_size - offset);
                sendto(m_udpSocket, (const char*)(framed + offset), cs, 0,
                       (sockaddr*)&m_serverAddr, sizeof(m_serverAddr));
                offset += cs;
            }
            free(framed);
        }
        free(fixed);
        DriverLog("[UDP] Fixed keyframe: inserted IDR start code at offset %d", pps_data_end);
        return;
    }

    // Frame the packet with a 4-byte big-endian length prefix so the receiver
    // can reconstruct the exact libx264 AVPacket (one full frame, including all
    // of its slices) regardless of UDP datagram boundaries. FFmpeg decodes the
    // in-band SPS/PPS/IDR Annex B stream directly.
    int framed_size = size + 4;
    uint8_t* framed = (uint8_t*)malloc(framed_size);
    if (framed) {
        framed[0] = (uint8_t)((size >> 24) & 0xFF);
        framed[1] = (uint8_t)((size >> 16) & 0xFF);
        framed[2] = (uint8_t)((size >> 8) & 0xFF);
        framed[3] = (uint8_t)(size & 0xFF);
        memcpy(framed + 4, data, size);

        int offset = 0;
        while (offset < framed_size) {
            int chunkSize = (framed_size - offset > 60000) ? 60000 : (framed_size - offset);
            sendto(m_udpSocket, (const char*)(framed + offset), chunkSize, 0,
                   (sockaddr*)&m_serverAddr, sizeof(m_serverAddr));
            offset += chunkSize;
        }
        free(framed);
    }
    DriverLog("[UDP] Sent framed packet: payload=%d bytes", size);
}

bool HmdDriver::InitializeUDP()
{
    DriverLog("Initializing UDP socket...");

    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        DriverLog("WSAStartup failed! Error: %d", result);
        return false;
    }

    m_udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_udpSocket == INVALID_SOCKET) {
        DriverLog("socket() failed! WSAError: %d", WSAGetLastError());
        WSACleanup();
        return false;
    }

    // Set socket buffer size
    int bufSize = 1024 * 1024; // 1MB send buffer
    setsockopt(m_udpSocket, SOL_SOCKET, SO_SNDBUF, (const char*)&bufSize, sizeof(bufSize));

    // Target: localhost:42069
    m_serverAddr.sin_family = AF_INET;
    m_serverAddr.sin_port = htons(UDP_SERVER_PORT);
    inet_pton(AF_INET, "127.0.0.1", &m_serverAddr.sin_addr);

    m_udpInitialized = true;
    DriverLog("UDP socket initialized. Sending to 127.0.0.1:%d", UDP_SERVER_PORT);
    return true;
}

void HmdDriver::ShutdownUDP()
{
    DriverLog("Shutting down UDP socket...");

    if (m_udpSocket != INVALID_SOCKET) {
        closesocket(m_udpSocket);
        m_udpSocket = INVALID_SOCKET;
    }

    m_udpInitialized = false;
    WSACleanup();
    DriverLog("UDP socket shutdown complete.");
}

bool HmdDriver::InitializeDiscovery()
{
    DriverLog("Initializing UDP discovery socket...");

    m_discoverySocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_discoverySocket == INVALID_SOCKET) {
        DriverLog("discovery socket() failed! WSAError: %d", WSAGetLastError());
        return false;
    }

    // Allow broadcast reception
    BOOL broadcast = TRUE;
    setsockopt(m_discoverySocket, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcast, sizeof(broadcast));

    // Allow address reuse
    BOOL reuseAddr = TRUE;
    setsockopt(m_discoverySocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuseAddr, sizeof(reuseAddr));

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(UDP_DISCOVERY_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(m_discoverySocket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        DriverLog("discovery bind() failed! WSAError: %d", WSAGetLastError());
        closesocket(m_discoverySocket);
        m_discoverySocket = INVALID_SOCKET;
        return false;
    }

    m_discoveryInitialized = true;
    m_discoveryRunning = true;

    m_discoveryThread = std::thread(&HmdDriver::DiscoveryThreadFunc, this);

    DriverLog("UDP discovery socket initialized. Listening on port %d", UDP_DISCOVERY_PORT);
    return true;
}

void HmdDriver::ShutdownDiscovery()
{
    DriverLog("Shutting down UDP discovery...");

    if (m_discoveryInitialized) {
        m_discoveryRunning = false;

        // Send a dummy packet to unblock recvfrom
        SOCKET wakeSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (wakeSocket != INVALID_SOCKET) {
            sockaddr_in localAddr;
            localAddr.sin_family = AF_INET;
            localAddr.sin_port = htons(UDP_DISCOVERY_PORT);
            inet_pton(AF_INET, "127.0.0.1", &localAddr.sin_addr);
            sendto(wakeSocket, "wake", 4, 0, (sockaddr*)&localAddr, sizeof(localAddr));
            closesocket(wakeSocket);
        }

        if (m_discoveryThread.joinable()) {
            m_discoveryThread.join();
        }

        if (m_discoverySocket != INVALID_SOCKET) {
            closesocket(m_discoverySocket);
            m_discoverySocket = INVALID_SOCKET;
        }

        m_discoveryInitialized = false;
    }

    DriverLog("UDP discovery shutdown complete.");
}

void HmdDriver::DiscoveryThreadFunc()
{
    DriverLog("Discovery thread started");

    char buffer[256];
    sockaddr_in senderAddr;
    int senderAddrLen = sizeof(senderAddr);

    while (m_discoveryRunning) {
        int bytesReceived = recvfrom(m_discoverySocket, buffer, sizeof(buffer) - 1, 0,
                                     (sockaddr*)&senderAddr, &senderAddrLen);

        if (!m_discoveryRunning) {
            break;
        }

        if (bytesReceived > 0) {
            buffer[bytesReceived] = '\0';
            char senderIpStr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &senderAddr.sin_addr, senderIpStr, sizeof(senderIpStr));

            DriverLog("Discovery packet received from %s:%d (size=%d, data='%s')",
                      senderIpStr, ntohs(senderAddr.sin_port), bytesReceived, buffer);

            if (strncmp(buffer, "CARDBOARD_CAP", 13) == 0) {
                // Phone is reporting its hardware decoder cap; no ACK needed.
                int capW = 0;
                int capH = 0;
                if (sscanf_s(buffer, "CARDBOARD_CAP %d %d", &capW, &capH) == 2) {
                    DriverLog("Hardware decoder cap received from %s: %dx%d", senderIpStr, capW, capH);
                    ApplyHardwareCap(capW, capH);
                }
                continue;
            }

            // Switch data target to the phone's IP
            SwitchDataTarget(senderIpStr);

            // Send acknowledgment back to the phone
            const char* ack = "ACK";
            sockaddr_in responseAddr;
            responseAddr.sin_family = AF_INET;
            responseAddr.sin_port = senderAddr.sin_port;
            responseAddr.sin_addr.s_addr = senderAddr.sin_addr.s_addr;
            sendto(m_discoverySocket, ack, 3, 0, (sockaddr*)&responseAddr, sizeof(responseAddr));

            DriverLog("Discovery ACK sent to %s", senderIpStr);
        }
    }

    DriverLog("Discovery thread exiting");
}

void HmdDriver::SwitchDataTarget(const char* phoneIp)
{
    std::lock_guard<std::mutex> lock(m_targetIpMutex);

    char currentIp[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &m_serverAddr.sin_addr, currentIp, sizeof(currentIp));

    if (strcmp(currentIp, phoneIp) == 0) {
        DriverLog("Data target already set to %s, skipping", phoneIp);
        return;
    }

    DriverLog("Switching data target from %s to %s", currentIp, phoneIp);

    m_serverAddr.sin_family = AF_INET;
    m_serverAddr.sin_port = htons(UDP_SERVER_PORT);
    inet_pton(AF_INET, phoneIp, &m_serverAddr.sin_addr);

    DriverLog("Data target switched to %s:%d", phoneIp, UDP_SERVER_PORT);
}
