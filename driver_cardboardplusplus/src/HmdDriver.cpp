#include "HmdDriver.h"
#include <dxgi.h>
#include <dxgi1_2.h>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cstring>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using namespace vr;

static const int UDP_SERVER_PORT = 42069;

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

void HmdDriver::OnEncodedPacket(uint8_t* data, int size, int64_t pts, bool keyframe)
{
    DriverLog("[Encoded] size=%d, pts=%lld, keyframe=%s, data[0]=0x%02X",
              size, pts, keyframe ? "YES" : "NO", data[0]);

    if (m_udpInitialized && m_udpSocket != INVALID_SOCKET && size > 0) {
        // Send in chunks (UDP max ~65507, but safer to use smaller chunks)
        int maxChunk = 60000;
        int offset = 0;
        while (offset < size) {
            int chunkSize = (size - offset > maxChunk) ? maxChunk : (size - offset);
            int sent = sendto(m_udpSocket, (const char*)(data + offset), chunkSize, 0,
                              (sockaddr*)&m_serverAddr, sizeof(m_serverAddr));
            if (sent == SOCKET_ERROR) {
                DriverLog("[UDP] sendto failed! WSAError: %d", WSAGetLastError());
                break;
            }
            offset += sent;
        }
    }
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
