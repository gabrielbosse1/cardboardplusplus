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

void DriverLog(const char* pFormat, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, pFormat);
    vsprintf_s(buffer, pFormat, args);
    strcat_s(buffer, "\n");
    vr::VRDriverLog()->Log(buffer);
    va_end(args);
}

// ============================================================
// Helper: get local IP address as string
// ============================================================
std::string HmdDriver::GetLocalIPAddress() {
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        return "127.0.0.1";
    }

    struct addrinfo hints = {}, *result = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

    // Use a dummy address to trigger route lookup
    if (getaddrinfo(hostname, nullptr, &hints, &result) != 0 || !result) {
        return "127.0.0.1";
    }

    char ipStr[INET_ADDRSTRLEN];
    struct sockaddr_in* addr = (struct sockaddr_in*)result->ai_addr;
    inet_ntop(AF_INET, &addr->sin_addr, ipStr, sizeof(ipStr));
    freeaddrinfo(result);

    return std::string(ipStr);
}

// ============================================================
// Activate: init everything
// ============================================================
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
    m_submitLayers[0] = { 0 };
    m_submitLayers[1] = { 0 };

    // Networking state
    m_wsaInitialized = false;
    m_broadcastSocket = INVALID_SOCKET;
    m_broadcastRunning = false;
    m_discoverySocket = INVALID_SOCKET;
    m_discoveryRunning = false;
    m_videoSocket = INVALID_SOCKET;
    m_videoTargetSet = false;
    m_trackingSocket = INVALID_SOCKET;
    m_trackingRunning = false;
    m_hasTracking = false;
    m_cameraSocket = INVALID_SOCKET;
    m_cameraRunning = false;
    m_latestTracking = {};

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

    if (!InitializeNetworking()) {
        DriverLog("WARNING: Networking initialization failed.");
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

// ============================================================
// Deactivate: clean up everything
// ============================================================
void HmdDriver::Deactivate()
{
	// Clean up resources and reset state.
    DriverLog("HmdDriver::Deactivate called");

    ShutdownNetworking();
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

// ============================================================
// GetPose: use phone tracking data when available, fallback to placeholder
// ============================================================
DriverPose_t HmdDriver::GetPose()
{
    DriverPose_t pose = { 0 };
    pose.poseIsValid = true;
    pose.result = TrackingResult_Running_OK;
    pose.deviceIsConnected = true;

    HmdQuaternion_t identity;
    identity.w = 1.0;
    identity.x = 0.0;
    identity.y = 0.0;
    identity.z = 0.0;

    pose.qWorldFromDriverRotation = identity;
    pose.qDriverFromHeadRotation = identity;

    // Use phone tracking data if available
    {
        std::lock_guard<std::mutex> lock(m_trackingMutex);
        if (m_hasTracking) {
            // Orientation from phone quaternion (raw, no transformation)
            pose.qRotation.w = m_latestTracking.orientationW;
            pose.qRotation.x = m_latestTracking.orientationX;
            pose.qRotation.y = m_latestTracking.orientationY;
            pose.qRotation.z = m_latestTracking.orientationZ;

            // Position from phone (with default height fallback)
            pose.vecPosition[0] = m_latestTracking.positionX;
            pose.vecPosition[1] = m_latestTracking.positionY;
            pose.vecPosition[2] = m_latestTracking.positionZ;

            return pose;
        }
    }

    // Fallback: gentle bob animation so SteamVR knows we're alive
    static auto startTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - startTime).count();

    float bobHeight = (float)(sin(elapsed * 2.0) * 0.02);
    float swayX = (float)(sin(elapsed * 1.5) * 0.01);

    pose.qRotation = identity;
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

// ============================================================
// IVRDisplayComponent
// ============================================================
void HmdDriver::GetWindowBounds( int32_t *pnX, int32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight )
{
    if (pnX) *pnX = 0;
    if (pnY) *pnY = 0;
    if (pnWidth) *pnWidth = 1920;
    if (pnHeight) *pnHeight = 1080;
}

bool HmdDriver::IsDisplayOnDesktop() { return false; }
bool HmdDriver::IsDisplayRealDisplay() { return false; }

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

// ============================================================
// IVRDriverDirectModeComponent
// ============================================================
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
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

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
                if (it->pKeyedMutex) it->pKeyedMutex->Release();
                if (it->pTexture) it->pTexture->Release();
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
            if (sts.pKeyedMutex) sts.pKeyedMutex->Release();
            if (sts.pTexture) sts.pTexture->Release();
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
        DriverLog("Encoding SBS: left=%llu right=%llu pts=%lld",
            (uint64_t)m_submitLayers[0].hTexture, (uint64_t)m_submitLayers[1].hTexture, m_encoderPts);
        m_pVideoEncoder->EncodeFrameSBS(pLeftTex, pRightTex, m_encoderPts);
        m_encoderPts++;
    } else if (pLeftTex) {
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

// ============================================================
// Video Encoder
// ============================================================
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

    // SBS resolution: 1440x1620 per eye -> 2880x1620
    int width = 2880;
    int height = 1620;
    int fps = 60;
    int bitrate = 20000000;
    bool useGpuEncoding = false;

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

// ============================================================
// OnEncodedPacket: send encoded H264 to phone via video socket
// ============================================================
void HmdDriver::OnEncodedPacket(uint8_t* data, int size, int64_t pts, bool keyframe)
{
    DriverLog("[Encoded] size=%d, pts=%lld, keyframe=%s, data[0]=0x%02X",
              size, pts, keyframe ? "YES" : "NO", data[0]);

    if (!m_wsaInitialized || m_videoSocket == INVALID_SOCKET || size <= 0) {
        return;
    }

    // Don't send if no phone is connected
    if (!m_videoTargetSet) {
        return;
    }

    // libx264 keyframes: insert IDR start code after PPS if missing
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
                // Check if IDR start code already present
                if (pps_data_end + 4 < size &&
                    data[pps_data_end] == 0x00 && data[pps_data_end+1] == 0x00 &&
                    data[pps_data_end+2] == 0x00 && data[pps_data_end+3] == 0x01 &&
                    (data[pps_data_end+4] & 0x1F) == 5) {
                    break; // IDR start code already present
                }
                needs_fix = true;
                break;
            }
        }
    }

    uint8_t* sendData = data;
    int sendSize = size;

    uint8_t* fixedBuf = nullptr;
    if (needs_fix && pps_data_end > 0) {
        sendSize = size + 5;
        fixedBuf = (uint8_t*)malloc(sendSize);
        memcpy(fixedBuf, data, pps_data_end);
        memcpy(fixedBuf + pps_data_end, idr_prefix, 5);
        memcpy(fixedBuf + pps_data_end + 5, data + pps_data_end, size - pps_data_end);
        sendData = fixedBuf;
        DriverLog("[UDP] Fixed keyframe: inserted IDR start code at offset %d", pps_data_end);
    }

    // Build protocol header
    cbpp::PacketHeader header;
    header.magic[0] = cbpp::MAGIC[0];
    header.magic[1] = cbpp::MAGIC[1];
    header.version = cbpp::PROTOCOL_VERSION;
    header.type = cbpp::PT_VIDEO_CHUNK;

    // Calculate chunks needed
    uint32_t totalChunks = (sendSize + cbpp::VIDEO_CHUNK_SIZE - 1) / cbpp::VIDEO_CHUNK_SIZE;
    static uint32_t frameCounter = 0;

    // Allocate send buffer once (avoid 60KB stack allocation)
    const int headerSize = sizeof(cbpp::PacketHeader) + sizeof(cbpp::VideoChunkHeader);
    uint8_t* sendBuf = (uint8_t*)malloc(headerSize + cbpp::VIDEO_CHUNK_SIZE);

    for (uint32_t chunkIdx = 0; chunkIdx < totalChunks; chunkIdx++) {
        uint32_t offset = chunkIdx * cbpp::VIDEO_CHUNK_SIZE;
        uint32_t chunkLen = sendSize - offset;
        if (chunkLen > cbpp::VIDEO_CHUNK_SIZE) chunkLen = cbpp::VIDEO_CHUNK_SIZE;

        cbpp::VideoChunkHeader chunkHeader;
        chunkHeader.frameId = frameCounter;
        chunkHeader.chunkIndex = chunkIdx;
        chunkHeader.totalChunks = totalChunks;
        chunkHeader.frameSize = (uint32_t)sendSize;
        chunkHeader.keyframe = keyframe ? 1 : 0;

        // Pack: [PacketHeader] [VideoChunkHeader] [data]
        header.payloadSize = sizeof(cbpp::VideoChunkHeader) + chunkLen;
        memcpy(sendBuf, &header, sizeof(cbpp::PacketHeader));
        memcpy(sendBuf + sizeof(cbpp::PacketHeader), &chunkHeader, sizeof(cbpp::VideoChunkHeader));
        memcpy(sendBuf + headerSize, sendData + offset, chunkLen);

        sendto(m_videoSocket, (const char*)sendBuf, headerSize + chunkLen, 0,
               (sockaddr*)&m_videoTargetAddr, sizeof(m_videoTargetAddr));
    }

    free(sendBuf);
    frameCounter++;

    if (fixedBuf) free(fixedBuf);
}

// ============================================================
// Networking: Initialize / Shutdown
// ============================================================
bool HmdDriver::InitializeNetworking()
{
    DriverLog("Initializing networking...");

    WSADATA wsa;
    int result = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (result != 0) {
        DriverLog("WSAStartup failed! Error: %d", result);
        return false;
    }
    m_wsaInitialized = true;

    if (!InitializeVideoSocket()) {
        DriverLog("WARNING: Video socket init failed");
    }

    if (!StartBroadcastThread()) {
        DriverLog("WARNING: Broadcast thread start failed");
    }

    if (!StartDiscoveryListener()) {
        DriverLog("WARNING: Discovery listener start failed");
    }

    if (!StartTrackingReceiver()) {
        DriverLog("WARNING: Tracking receiver start failed");
    }

    if (!StartCameraReceiver()) {
        DriverLog("WARNING: Camera receiver start failed");
    }

    std::string localIp = GetLocalIPAddress();
    DriverLog("Local IP: %s", localIp.c_str());
    DriverLog("Networking initialized. Broadcast=%d, Discovery=%d, Video=%d, Tracking=%d, Camera=%d",
              m_broadcastRunning.load(), m_discoveryRunning.load(), m_videoSocket != INVALID_SOCKET,
              m_trackingRunning.load(), m_cameraRunning.load());

    return true;
}

void HmdDriver::ShutdownNetworking()
{
    DriverLog("Shutting down networking...");

    // Stop broadcast thread
    m_broadcastRunning = false;
    if (m_broadcastSocket != INVALID_SOCKET) {
        closesocket(m_broadcastSocket);
        m_broadcastSocket = INVALID_SOCKET;
    }
    if (m_broadcastThread.joinable()) m_broadcastThread.join();

    // Stop discovery listener
    m_discoveryRunning = false;
    if (m_discoverySocket != INVALID_SOCKET) {
        closesocket(m_discoverySocket);
        m_discoverySocket = INVALID_SOCKET;
    }
    if (m_discoveryThread.joinable()) m_discoveryThread.join();

    // Stop tracking receiver
    m_trackingRunning = false;
    if (m_trackingSocket != INVALID_SOCKET) {
        closesocket(m_trackingSocket);
        m_trackingSocket = INVALID_SOCKET;
    }
    if (m_trackingThread.joinable()) m_trackingThread.join();

    // Stop camera receiver
    m_cameraRunning = false;
    if (m_cameraSocket != INVALID_SOCKET) {
        closesocket(m_cameraSocket);
        m_cameraSocket = INVALID_SOCKET;
    }
    if (m_cameraThread.joinable()) m_cameraThread.join();

    ShutdownVideoSocket();

    m_wsaInitialized = false;
    WSACleanup();
    DriverLog("Networking shutdown complete.");
}

// ============================================================
// Broadcast: periodically announce driver on LAN
// ============================================================
bool HmdDriver::StartBroadcastThread()
{
    m_broadcastSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_broadcastSocket == INVALID_SOCKET) {
        DriverLog("Broadcast socket() failed! Error: %d", WSAGetLastError());
        return false;
    }

    int broadcastEnable = 1;
    setsockopt(m_broadcastSocket, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcastEnable, sizeof(broadcastEnable));

    int bufSize = 65536;
    setsockopt(m_broadcastSocket, SOL_SOCKET, SO_SNDBUF, (const char*)&bufSize, sizeof(bufSize));

    m_broadcastRunning = true;
    m_broadcastThread = std::thread(&HmdDriver::BroadcastLoop, this);
    DriverLog("Broadcast thread started on port %d", cbpp::PORT_BROADCAST);
    return true;
}

void HmdDriver::BroadcastLoop()
{
    sockaddr_in broadcastAddr = {};
    broadcastAddr.sin_family = AF_INET;
    broadcastAddr.sin_port = htons(cbpp::PORT_BROADCAST);
    broadcastAddr.sin_addr.s_addr = INADDR_BROADCAST;

    std::string localIp = GetLocalIPAddress();

    while (m_broadcastRunning) {
        cbpp::PacketHeader header = {};
        header.magic[0] = cbpp::MAGIC[0];
        header.magic[1] = cbpp::MAGIC[1];
        header.version = cbpp::PROTOCOL_VERSION;
        header.type = cbpp::PT_DISCOVERY_ANNOUNCE;
        header.payloadSize = sizeof(cbpp::AnnouncePayload);

        cbpp::AnnouncePayload payload = {};
        payload.videoPort = cbpp::PORT_VIDEO;
        payload.cameraPort = cbpp::PORT_CAMERA;
        payload.trackingPort = cbpp::PORT_TRACKING;

        // Convert our IP to network byte order
        struct in_addr addr;
        inet_pton(AF_INET, localIp.c_str(), &addr);
        payload.serverIp = addr.S_un.S_addr;

        strncpy_s(payload.name, "CardboardPlusPlus", sizeof(payload.name) - 1);

        uint8_t buf[sizeof(cbpp::PacketHeader) + sizeof(cbpp::AnnouncePayload)];
        memcpy(buf, &header, sizeof(cbpp::PacketHeader));
        memcpy(buf + sizeof(cbpp::PacketHeader), &payload, sizeof(cbpp::AnnouncePayload));

        sendto(m_broadcastSocket, (const char*)buf, sizeof(buf), 0,
               (sockaddr*)&broadcastAddr, sizeof(broadcastAddr));

        Sleep(cbpp::BROADCAST_INTERVAL_MS);
    }
}

// ============================================================
// Discovery Listener: listen for phone responses
// ============================================================
bool HmdDriver::StartDiscoveryListener()
{
    m_discoverySocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_discoverySocket == INVALID_SOCKET) {
        DriverLog("Discovery socket() failed! Error: %d", WSAGetLastError());
        return false;
    }

    int reuse = 1;
    setsockopt(m_discoverySocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in bindAddr = {};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(cbpp::PORT_BROADCAST);
    bindAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(m_discoverySocket, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
        DriverLog("Discovery bind() failed! Error: %d", WSAGetLastError());
        closesocket(m_discoverySocket);
        m_discoverySocket = INVALID_SOCKET;
        return false;
    }

    // Set non-blocking
    u_long mode = 1;
    ioctlsocket(m_discoverySocket, FIONBIO, &mode);

    m_discoveryRunning = true;
    m_discoveryThread = std::thread(&HmdDriver::DiscoveryListenerLoop, this);
    DriverLog("Discovery listener started on port %d", cbpp::PORT_BROADCAST);
    return true;
}

void HmdDriver::DiscoveryListenerLoop()
{
    uint8_t buf[1024];
    sockaddr_in fromAddr = {};
    int fromLen = sizeof(fromAddr);

    while (m_discoveryRunning) {
        int received = recvfrom(m_discoverySocket, (char*)buf, sizeof(buf), 0,
                                (sockaddr*)&fromAddr, &fromLen);

        if (received > 0 && received >= (int)sizeof(cbpp::PacketHeader)) {
            cbpp::PacketHeader* hdr = (cbpp::PacketHeader*)buf;

            if (hdr->magic[0] != cbpp::MAGIC[0] || hdr->magic[1] != cbpp::MAGIC[1]) {
                Sleep(10);
                continue;
            }

            if (hdr->type == cbpp::PT_DISCOVERY_RESPONSE) {
                if (received >= (int)(sizeof(cbpp::PacketHeader) + sizeof(cbpp::ResponsePayload))) {
                    cbpp::ResponsePayload* resp = (cbpp::ResponsePayload*)(buf + sizeof(cbpp::PacketHeader));

                    std::lock_guard<std::mutex> lock(m_clientMutex);
                    m_client.connected = true;
                    m_client.addr = fromAddr;
                    m_client.addr.sin_port = htons(cbpp::PORT_VIDEO);

                    // Set video target to the phone
                    m_videoTargetAddr = fromAddr;
                    m_videoTargetAddr.sin_port = htons(cbpp::PORT_VIDEO);
                    m_videoTargetSet = true;

                    char ipStr[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &fromAddr.sin_addr, ipStr, sizeof(ipStr));
                    DriverLog("Phone connected: %s", ipStr);
                }
            }
        }

        Sleep(10);
    }
}

// ============================================================
// Video Socket: send H264 frames to phone
// ============================================================
bool HmdDriver::InitializeVideoSocket()
{
    m_videoSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_videoSocket == INVALID_SOCKET) {
        DriverLog("Video socket() failed! Error: %d", WSAGetLastError());
        return false;
    }

    int bufSize = 1024 * 1024; // 1MB send buffer
    setsockopt(m_videoSocket, SOL_SOCKET, SO_SNDBUF, (const char*)&bufSize, sizeof(bufSize));

    DriverLog("Video socket initialized on port %d", cbpp::PORT_VIDEO);
    return true;
}

void HmdDriver::ShutdownVideoSocket()
{
    if (m_videoSocket != INVALID_SOCKET) {
        closesocket(m_videoSocket);
        m_videoSocket = INVALID_SOCKET;
    }
    m_videoTargetSet = false;
}

// ============================================================
// Tracking Receiver: receive orientation/position from phone
// ============================================================
bool HmdDriver::StartTrackingReceiver()
{
    m_trackingSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_trackingSocket == INVALID_SOCKET) {
        DriverLog("Tracking socket() failed! Error: %d", WSAGetLastError());
        return false;
    }

    int reuse = 1;
    setsockopt(m_trackingSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in bindAddr = {};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(cbpp::PORT_TRACKING);
    bindAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(m_trackingSocket, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
        DriverLog("Tracking bind() failed! Error: %d", WSAGetLastError());
        closesocket(m_trackingSocket);
        m_trackingSocket = INVALID_SOCKET;
        return false;
    }

    u_long mode = 1;
    ioctlsocket(m_trackingSocket, FIONBIO, &mode);

    m_trackingRunning = true;
    m_trackingThread = std::thread(&HmdDriver::TrackingReceiverLoop, this);
    DriverLog("Tracking receiver started on port %d", cbpp::PORT_TRACKING);
    return true;
}

void HmdDriver::TrackingReceiverLoop()
{
    uint8_t buf[1024];
    sockaddr_in fromAddr = {};
    int fromLen = sizeof(fromAddr);

    while (m_trackingRunning) {
        int received = recvfrom(m_trackingSocket, (char*)buf, sizeof(buf), 0,
                                (sockaddr*)&fromAddr, &fromLen);

        if (received >= (int)(sizeof(cbpp::PacketHeader) + sizeof(cbpp::TrackingPayload))) {
            cbpp::PacketHeader* hdr = (cbpp::PacketHeader*)buf;

            if (hdr->magic[0] == cbpp::MAGIC[0] && hdr->magic[1] == cbpp::MAGIC[1] &&
                hdr->type == cbpp::PT_TRACKING) {
                cbpp::TrackingPayload* track = (cbpp::TrackingPayload*)(buf + sizeof(cbpp::PacketHeader));

                std::lock_guard<std::mutex> lock(m_trackingMutex);
                m_latestTracking = *track;
                m_hasTracking = true;
            }
        }

        Sleep(2); // ~500 Hz polling, low latency
    }
}

// ============================================================
// Camera Receiver: receive camera frames from phone (placeholder)
// ============================================================
bool HmdDriver::StartCameraReceiver()
{
    m_cameraSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_cameraSocket == INVALID_SOCKET) {
        DriverLog("Camera socket() failed! Error: %d", WSAGetLastError());
        return false;
    }

    int reuse = 1;
    setsockopt(m_cameraSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in bindAddr = {};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(cbpp::PORT_CAMERA);
    bindAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(m_cameraSocket, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
        DriverLog("Camera bind() failed! Error: %d", WSAGetLastError());
        closesocket(m_cameraSocket);
        m_cameraSocket = INVALID_SOCKET;
        return false;
    }

    u_long mode = 1;
    ioctlsocket(m_cameraSocket, FIONBIO, &mode);

    m_cameraRunning = true;
    m_cameraThread = std::thread(&HmdDriver::CameraReceiverLoop, this);
    DriverLog("Camera receiver started on port %d (placeholder)", cbpp::PORT_CAMERA);
    return true;
}

void HmdDriver::CameraReceiverLoop()
{
    uint8_t buf[65536];
    sockaddr_in fromAddr = {};
    int fromLen = sizeof(fromAddr);

    while (m_cameraRunning) {
        int received = recvfrom(m_cameraSocket, (char*)buf, sizeof(buf), 0,
                                (sockaddr*)&fromAddr, &fromLen);

        if (received >= (int)sizeof(cbpp::PacketHeader)) {
            cbpp::PacketHeader* hdr = (cbpp::PacketHeader*)buf;

            if (hdr->magic[0] == cbpp::MAGIC[0] && hdr->magic[1] == cbpp::MAGIC[1] &&
                hdr->type == cbpp::PT_CAMERA_CHUNK) {
                // TODO: decode camera frames when real data arrives
                DriverLog("[Camera] Received chunk: %d bytes (placeholder)", received);
            }
        }

        Sleep(10);
    }
}
