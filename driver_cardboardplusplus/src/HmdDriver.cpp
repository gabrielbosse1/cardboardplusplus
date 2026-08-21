#include "HmdDriver.h"
#include "DriverLog.h"
#include <chrono>
#include <cmath>
#include <cstring>

using namespace vr;

// ---------------------------------------------------------------------------
// Device lifecycle + the SteamVR-facing probe/pose/display methods.
//
// Everything that touches real rendering, encoding, or networking lives in its
// own translation unit (see the header for the full map); this file only
// answers "who is this device" questions and drives its activate/teardown.
// ---------------------------------------------------------------------------

// iGPU test (Intel IntelArc/UHD): stop handing SteamVR the direct-display-mode
// present handshake by not exposing IVRDriverDirectModeComponent and advertising
// Prop_HasDriverDirectModeComponent=false. Nothing will be streamed in this mode;
// the encoder is fed exclusively through the Present path. Comment the line out
// to restore direct mode.
// #define DRIVER_NO_DIRECT_MODE

EVRInitError HmdDriver::Activate(uint32_t unObjectId)
{
    // When I wrote this code, only God and I understood it.
    // Now only God understands it.
    // If you're an atheist, good luck.
    // I even managed to somehow get the error "'cannot open file 'kernel32.lib'"
    m_driverId = unObjectId;
    m_encoderInitialized = false;
    m_encoderPts = 0;
    m_pVideoEncoder = nullptr;
    m_hasSubmit = false;
    m_udpSocket = INVALID_SOCKET;
    m_udpInitialized = false;
    m_udpDroppedFrames = 0;
    m_discoverySocket = INVALID_SOCKET;
    m_discoveryInitialized = false;
    m_discoveryRunning = false;
    m_submitLayers.clear();
    m_cachedSyncHandle = nullptr;
    m_pSyncTexture = nullptr;
    m_pSyncMutex = nullptr;
    m_frameQueued = false;
    m_encodeDone = true;
    m_pendingFrame = { nullptr, nullptr, 0, false };

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
        &m_pD3D11Device,
        &featureLevel,
        &m_pD3D11DeviceContext
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

    // Start background encoding thread. All slow work (GPU readback, pixel
    // conversion, encode, UDP send) happens here so Present() returns quickly
    // and the SteamVR compositor keeps its vsync pacing.
    m_encodingRunning = true;
    m_encodingThread = std::thread(&HmdDriver::EncodingThreadFunc, this);
    DriverLog("Background encoding thread started");

    if (!InitializeBridge()) {
        DriverLog("WARNING: Bridge shared-memory initialization failed. Telemetry disabled.");
    }

    // Set HMD properties
    PropertyContainerHandle_t props = VRProperties()->TrackedDeviceToPropertyContainer(m_driverId);

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
#ifdef DRIVER_NO_DIRECT_MODE
    VRProperties()->SetBoolProperty(props, Prop_HasDriverDirectModeComponent_Bool, false);
    DriverLog("HMD properties set: HasDriverDirectModeComponent=false (DIRECT MODE DISABLED), IsDisplayOnDesktop=false, DebugMode=false");
#else
    VRProperties()->SetBoolProperty(props, Prop_HasDriverDirectModeComponent_Bool, true);
    DriverLog("HMD properties set: HasDriverDirectModeComponent=true, IsDisplayOnDesktop=false, DebugMode=false");
#endif

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

    VRServerDriverHost()->SetDisplayEyeToHead(m_driverId, eyeToHeadLeft, eyeToHeadRight);

    return VRInitError_None;
}

void HmdDriver::Deactivate()
{
	// Clean up resources and reset state.
    DriverLog("HmdDriver::Deactivate called");

    // Stop background encoding thread and wait for any in-flight frame.
    m_encodingRunning = false;
    m_encodeCv.notify_all();
    if (m_encodingThread.joinable()) {
        m_encodingThread.join();
    }
    DriverLog("Background encoding thread stopped");

    ReleaseSyncTexture();
    if (m_pSyncMutex) {
        m_pSyncMutex->Release();
        m_pSyncMutex = nullptr;
    }
    if (m_pSyncTexture) {
        m_pSyncTexture->Release();
        m_pSyncTexture = nullptr;
    }
    m_cachedSyncHandle = nullptr;
    m_syncAcquired = false;

    // Release per-layer private eye copies (and their shared handles)
    for (auto& c : m_layerCopies) {
        if (c.pLeft) { c.pLeft->Release(); c.pLeft = nullptr; }
        if (c.pRight) { c.pRight->Release(); c.pRight = nullptr; }
        if (c.hLeft) { CloseHandle(c.hLeft); c.hLeft = nullptr; }
        if (c.hRight) { CloseHandle(c.hRight); c.hRight = nullptr; }
    }
    m_layerCopies.clear();

    ShutdownDiscovery();
    ShutdownUDP();
    ShutdownVideoEncoder();
    DestroyAllSwapTextureSets(0);

    if (m_pD3D11DeviceContext) {
        m_pD3D11DeviceContext->Release();
        m_pD3D11DeviceContext = nullptr;
    }
    if (m_pD3D11Device) {
        m_pD3D11Device->Release();
        m_pD3D11Device = nullptr;
    }
    m_driverId = k_unTrackedDeviceIndexInvalid;
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
#ifndef DRIVER_NO_DIRECT_MODE
    if (strcmp(pchComponentNameAndVersion, IVRDriverDirectModeComponent_Version) == 0)
    {
        return static_cast<IVRDriverDirectModeComponent*>(this);
    }
#endif
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
    VRServerDriverHost()->TrackedDevicePoseUpdated(m_driverId, pose, sizeof(DriverPose_t));

    if (m_bridgeInitialized.load(std::memory_order_relaxed)) {
        cbpp::PayloadSettingsChange s;
        if (m_bridgeServer.PollSettings(s)) {
            ApplyStreamSettings(s);
        }
        RunBridgeHeartbeat();
    }
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

void HmdDriver::RunBridgeHeartbeat()
{
    long long nowNs = std::chrono::steady_clock::now().time_since_epoch().count();
    if (m_lastHeartbeatNs == 0) {
        m_lastHeartbeatNs = nowNs;
        return;
    }
    if (nowNs - m_lastHeartbeatNs >= 1'000'000'000LL) {
        m_lastHeartbeatNs = nowNs;
        if (m_bridgeServer.running()) {
            m_bridgeServer.PublishStatus();
        }
    }
}

void HmdDriver::ApplyStreamSettings(const cbpp::PayloadSettingsChange& settings)
{
    std::lock_guard<std::mutex> lock(m_encoderMutex);

    m_streamEnabled.store(settings.stream_enabled ? 1 : 0, std::memory_order_relaxed);
    DriverLog("Bridge settings (seq=%llu): %ux%u @%u fps, %u kbps, encoder=%u, stream=%s",
              (unsigned long long)settings.seq, settings.width, settings.height, settings.fps,
              settings.bitrate_kbps, settings.encoder,
              settings.stream_enabled ? "ON" : "OFF");

    bool oldGpu = m_encoderUseGpu;
    int oldW = m_encoderW;
    int oldH = m_encoderH;
    int oldFps = m_encoderFps;
    int oldBitrate = m_encoderBitrate;

    m_encoderW = (int)settings.width;
    m_encoderH = (int)settings.height;
    m_encoderFps = (int)settings.fps;
    m_encoderBitrate = (int)settings.bitrate_kbps * 1000;
    m_encoderUseGpu = (settings.encoder != 0);
    ClampEncoderToCap();

    if (!m_encoderInitialized || !m_pVideoEncoder) {
        DriverLog("Bridge settings stored; encoder not up yet, applied on init.");
        return;
    }

    bool meaningful = (m_encoderW != oldW || m_encoderH != oldH || m_encoderFps != oldFps ||
                       m_encoderBitrate != oldBitrate || m_encoderUseGpu != oldGpu);
    if (!meaningful)
        return;

    DriverLog("Re-initializing encoder at %dx%d @%d fps, %d kbps, gpu=%d (from bridge settings)",
              m_encoderW, m_encoderH, m_encoderFps, m_encoderBitrate / 1000, m_encoderUseGpu ? 1 : 0);

    m_pVideoEncoder->Shutdown();
    delete m_pVideoEncoder;
    m_pVideoEncoder = nullptr;
    m_encoderInitialized = false;

    m_pVideoEncoder = new VideoEncoder();
    if (!m_pVideoEncoder) {
        DriverLog("Failed to allocate VideoEncoder during bridge settings re-init!");
        return;
    }

    m_pVideoEncoder->SetEncodedPacketCallback([this](uint8_t* data, int size, int64_t pts, bool keyframe) {
        OnEncodedPacket(data, size, pts, keyframe);
    });
    m_pVideoEncoder->SetTelemetryCallback([this](const cbpp::PayloadTelemetry& t) {
        if (m_bridgeInitialized.load(std::memory_order_relaxed) && m_bridgeServer.running()) {
            m_bridgeServer.PublishTelemetry(t);
        }
    });

    if (!m_pVideoEncoder->Initialize(m_pD3D11Device, m_pD3D11DeviceContext,
                                     m_encoderW, m_encoderH, m_encoderFps, m_encoderBitrate, m_encoderUseGpu)) {
        DriverLog("VideoEncoder re-init failed under bridge settings!");
        delete m_pVideoEncoder;
        m_pVideoEncoder = nullptr;
        return;
    }

    m_encoderInitialized = true;
    m_encoderPts = 0;
    DriverLog("Encoder re-initialized at %dx%d from bridge settings", m_encoderW, m_encoderH);
}

bool HmdDriver::InitializeBridge()
{
    if (m_bridgeServer.Start()) {
        m_bridgeInitialized.store(true, std::memory_order_relaxed);
        DriverLog("Bridge shared-memory region created at %ls", cbpp::kRegionName);
        m_bridgeServer.PublishStatus();
        return true;
    }
    m_bridgeInitialized.store(false, std::memory_order_relaxed);
    return false;
}

void HmdDriver::ShutdownBridge()
{
    m_bridgeInitialized.store(false, std::memory_order_relaxed);
    m_bridgeServer.ShutdownCmdConsumer();
    m_bridgeServer.Stop();
    DriverLog("Bridge shared-memory regions released");
}
