#include "HmdDriver.h"
#include "DriverLog.h"
#include "DebugLog.h"
#include "CardboardWire.h"
#include <chrono>
#include <cstring>
#include <fstream>
using namespace vr;
// Called by SteamVR to register the HMD; creates D3D, encoder, UDP/discovery/sensor sockets, encoding thread, bridge SHM, and device properties.
EVRInitError HmdDriver::Activate(uint32_t unObjectId)
{
    m_driverId = unObjectId;
    m_encoderInitialized = false;
    m_encoderPts = 0;
    m_pVideoEncoder = nullptr;
    m_hasSubmit = false;
    m_udpSocket = INVALID_SOCKET;
    m_udpInitialized = false;
    m_udpDroppedPreview = 0;
    m_udpDroppedPhone = 0;
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
    m_streamEnabled.store(1, std::memory_order_relaxed);
    m_presentCount = 0;
    m_lastPresentLogNs = 0;
    m_lastHeartbeatNs = 0;
    m_submitLayers.clear();
    DriverLog("HmdDriver::Activate called");
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
    if (!InitializeSensorSocket()) {
        DriverLog("WARNING: Sensor socket initialization failed. Head tracking will use synthetic data.");
    }
    m_encodingRunning = true;
    m_encodingThread = std::thread(&HmdDriver::EncodingThreadFunc, this);
    DriverLog("Background encoding thread started");
    if (!InitializeBridge()) {
        DriverLog("WARNING: Bridge shared-memory initialization failed. Telemetry disabled.");
    }
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
    VRProperties()->SetBoolProperty(props, Prop_DeviceProvidesBatteryStatus_Bool, false);
    VRDriverInput()->CreateBooleanComponent(props, "/proximity", &m_proximityHandle);
    VRDriverInput()->UpdateBooleanComponent(m_proximityHandle, true, 0);
#ifdef DRIVER_NO_DIRECT_MODE
    VRProperties()->SetBoolProperty(props, Prop_HasDriverDirectModeComponent_Bool, false);
    DriverLog("HMD properties set: HasDriverDirectModeComponent=false (DIRECT MODE DISABLED), IsDisplayOnDesktop=false, DebugMode=false");
#else
    VRProperties()->SetBoolProperty(props, Prop_HasDriverDirectModeComponent_Bool, true);
    DriverLog("HMD properties set: HasDriverDirectModeComponent=true, IsDisplayOnDesktop=false, DebugMode=false");
#endif
    HmdMatrix34_t eyeToHeadLeft = { 0 };
    eyeToHeadLeft.m[0][0] = 1.0f;
    eyeToHeadLeft.m[1][1] = 1.0f;
    eyeToHeadLeft.m[2][2] = 1.0f;
    eyeToHeadLeft.m[0][3] = -0.032f;
    HmdMatrix34_t eyeToHeadRight = { 0 };
    eyeToHeadRight.m[0][0] = 1.0f;
    eyeToHeadRight.m[1][1] = 1.0f;
    eyeToHeadRight.m[2][2] = 1.0f;
    eyeToHeadRight.m[0][3] = 0.032f;
    VRServerDriverHost()->SetDisplayEyeToHead(m_driverId, eyeToHeadLeft, eyeToHeadRight);
    return VRInitError_None;
}
// Called by SteamVR at unload; joins threads, releases textures/sockets/encoder/bridge, then drops the D3D device.
void HmdDriver::Deactivate()
{
    DriverLog("HmdDriver::Deactivate called");
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
    for (auto& c : m_layerCopies) {
        if (c.pLeft) { c.pLeft->Release(); c.pLeft = nullptr; }
        if (c.pRight) { c.pRight->Release(); c.pRight = nullptr; }
        if (c.hLeft) { CloseHandle(c.hLeft); c.hLeft = nullptr; }
        if (c.hRight) { CloseHandle(c.hRight); c.hRight = nullptr; }
    }
    m_layerCopies.clear();
    ShutdownDiscovery();
    ShutdownSensorSocket();
    ShutdownUDP();
    ShutdownVideoEncoder();
    ShutdownBridge();
    std::vector<uint32_t> pids;
    pids.reserve(m_swapTextureSets.size());
    for (const auto& kv : m_swapTextureSets) {
        pids.push_back(kv.first);
    }
    for (uint32_t pid : pids) {
        DestroyAllSwapTextureSets(pid);
    }
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
// Standby hook (no-op); SteamVR calls it when the HMD would sleep.
// Exposes IVRDisplayComponent and IVRDriverDirectModeComponent to the compositor; null for other interfaces.
void HmdDriver::EnterStandby() {}
// Exposes IVRDisplayComponent and IVRDriverDirectModeComponent to the compositor; null for other interfaces.
void* HmdDriver::GetComponent(const char* pchComponentNameAndVersion)
{
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
// Answers SteamVR debug console queries with an empty string; pchRequest selects the query.
// Called by SteamVR each frame; reports the phone quaternion when fresh, otherwise an out-of-range pose.
void HmdDriver::DebugRequest(const char* pchRequest, char* pchResponseBuffer, uint32_t unResponseBufferSize)
{
    if (unResponseBufferSize >= 1)
    {
        pchResponseBuffer[0] = 0;
    }
}
// Called by SteamVR each frame; reports the phone quaternion when fresh, otherwise an out-of-range pose.
DriverPose_t HmdDriver::GetPose()
{
    DriverPose_t pose = { 0 };
    pose.poseIsValid = true;
    pose.result = TrackingResult_Running_OK;
    pose.deviceIsConnected = true;
    pose.qWorldFromDriverRotation.w = 1.0;
    pose.qWorldFromDriverRotation.x = 0.0;
    pose.qWorldFromDriverRotation.y = 0.0;
    pose.qWorldFromDriverRotation.z = 0.0;
    static constexpr int64_t kSensorStaleMs = 2000;
    const int64_t lastRecv = m_lastSensorRecvMs.load(std::memory_order_relaxed);
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const bool fresh = lastRecv != 0 && (nowMs - lastRecv) < kSensorStaleMs;
    const int64_t lastRot = m_lastRotationRecvMs.load(std::memory_order_relaxed);
    const bool rotFresh = lastRot != 0 && (nowMs - lastRot) < kSensorStaleMs;
    bool hasQ = rotFresh && m_hasQuaternion.load(std::memory_order_relaxed);
    if (!fresh) {
        pose.poseIsValid = false;
        pose.result = TrackingResult_Running_OutOfRange;
        pose.qDriverFromHeadRotation.w = 1.0;
        pose.qDriverFromHeadRotation.x = 0.0;
        pose.qDriverFromHeadRotation.y = 0.0;
        pose.qDriverFromHeadRotation.z = 0.0;
        pose.qRotation = pose.qDriverFromHeadRotation;
        return pose;
    }
    HmdQuaternion_t quat;
    quat.w = 1.0;
    quat.x = 0.0;
    quat.y = 0.0;
    quat.z = 0.0;
    if (hasQ) {
        std::lock_guard<std::mutex> lock(m_sensorMutex);
        quat.w = m_sensorQuat[0];
        quat.x = m_sensorQuat[1];
        quat.y = m_sensorQuat[2];
        quat.z = m_sensorQuat[3];
    }
    pose.qDriverFromHeadRotation.w = 1.0;
    pose.qDriverFromHeadRotation.x = 0.0;
    pose.qDriverFromHeadRotation.y = 0.0;
    pose.qDriverFromHeadRotation.z = 0.0;
    pose.qRotation = quat;
    return pose;
}
// Called by DeviceProvider each frame; publishes the pose, drains bridge settings, and emits the 1 Hz status publish.
void HmdDriver::RunFrame()
{
    DriverPose_t pose = GetPose();
    VRServerDriverHost()->TrackedDevicePoseUpdated(m_driverId, pose, sizeof(DriverPose_t));
    {
        static int rfCount = 0;
        rfCount++;
        if (rfCount % 300 == 1) {
            DriverLog("RunFrame #%d driverId=%u hasQ=%d", rfCount, m_driverId,
                      m_hasQuaternion.load(std::memory_order_relaxed) ? 1 : 0);
        }
    }
    if (m_bridgeInitialized.load(std::memory_order_relaxed)) {
        cbpp::PayloadSettingsChange s;
        if (m_bridgeServer.PollSettings(s)) {
            ApplyStreamSettings(s);
        }
        RunBridgeHeartbeat();
    }
}
// Reports the virtual window origin/size to SteamVR; pnX/pnY/pnWidth/pnHeight receive 0,0,1920,1080.
void HmdDriver::GetWindowBounds( int32_t *pnX, int32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight )
{
    if (pnX) *pnX = 0;
    if (pnY) *pnY = 0;
    if (pnWidth) *pnWidth = 1920;
    if (pnHeight) *pnHeight = 1080;
}
// Reports that the display is a direct-mode panel rather than a desktop mirror.
// Reports that the display is virtual rather than a physical monitor.
bool HmdDriver::IsDisplayOnDesktop()
{
    return false;
}
// Reports that the display is virtual rather than a physical monitor.
bool HmdDriver::IsDisplayRealDisplay()
{
    return false;
}
// Provides the per-eye render size (960x1080 half-SBS) the VR app should render into.
// Provides the left/right viewport rectangles tiling the 1920x1080 SBS frame; eEye selects the half.
void HmdDriver::GetRecommendedRenderTargetSize( uint32_t *pnWidth, uint32_t *pnHeight )
{
    if (pnWidth) *pnWidth = 1920 / 2;
    if (pnHeight) *pnHeight = 1080;
}
// Provides the left/right viewport rectangles tiling the 1920x1080 SBS frame; eEye selects the half.
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
// Provides a symmetric 90-degree frustum per eye; eEye is ignored because both eyes share it.
// Passes UVs through unchanged; the phone performs lens correction in its renderer.
void HmdDriver::GetProjectionRaw( EVREye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom )
{
    *pfLeft = -1.0;
    *pfRight = 1.0;
    *pfTop = -1.0;
    *pfBottom = 1.0;
}
// Passes UVs through unchanged; the phone performs lens correction in its renderer.
DistortionCoordinates_t HmdDriver::ComputeDistortion( EVREye eEye, float fU, float fV )
{
    DistortionCoordinates_t coordinates;
    coordinates.rfBlue[0] = fU;
    coordinates.rfBlue[1] = fV;
    coordinates.rfGreen[0] = fU;
    coordinates.rfGreen[1] = fV;
    coordinates.rfRed[0] = fU;
    coordinates.rfRed[1] = fV;
    return coordinates;
}
// Republishes the cached telemetry once per second; called from RunFrame when the bridge region is live.
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
// Applies a bridge settings slot: toggles streaming and forwards geometry/bitrate/encoder choice to ApplyEncoderSettings.
void HmdDriver::ApplyStreamSettings(const cbpp::PayloadSettingsChange& settings)
{
    int newStream = settings.stream_enabled ? 1 : 0;
    int oldStream = m_streamEnabled.exchange(newStream, std::memory_order_relaxed);
    if (newStream != oldStream) {
        DriverLog("Bridge stream switch: %s -> %s",
                  oldStream ? "ON" : "OFF", newStream ? "ON" : "OFF");
    }
    DriverLog("Bridge settings (seq=%llu): %ux%u @%u fps, %u kbps, encoder=%u, stream=%s",
              (unsigned long long)settings.seq, settings.width, settings.height, settings.fps,
              settings.bitrate_kbps, settings.encoder,
              settings.stream_enabled ? "ON" : "OFF");
    int bps = (settings.bitrate_kbps >= 1 && settings.bitrate_kbps <= 100000)
        ? (int)settings.bitrate_kbps * 1000 : -1;
    ApplyEncoderSettings((int)settings.width, (int)settings.height, (int)settings.fps,
                         bps, settings.encoder != 0, "bridge-settings");
}
// Creates the telemetry SHM region and emits an initial status; called from Activate.
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
// Detaches the command consumer and destroys the telemetry region; called from Deactivate.
void HmdDriver::ShutdownBridge()
{
    m_bridgeInitialized.store(false, std::memory_order_relaxed);
    m_bridgeServer.ShutdownCmdConsumer();
    m_bridgeServer.Stop();
    DriverLog("Bridge shared-memory regions released");
}
// Binds the sensor UDP socket and starts the sensor thread; called from Activate.
bool HmdDriver::InitializeSensorSocket()
{
    DriverLog("Initializing sensor socket...");
    m_sensorSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_sensorSocket == INVALID_SOCKET) {
        DriverLog("sensor socket() failed! WSAError: %d", WSAGetLastError());
        return false;
    }
    BOOL reuseAddr = TRUE;
    setsockopt(m_sensorSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuseAddr, sizeof(reuseAddr));
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(wire::kSensorPort);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(m_sensorSocket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        DriverLog("sensor bind() failed on port %d! WSAError: %d", wire::kSensorPort, WSAGetLastError());
        closesocket(m_sensorSocket);
        m_sensorSocket = INVALID_SOCKET;
        return false;
    }
    m_sensorInitialized = true;
    m_sensorRunning = true;
    m_sensorThread = std::thread(&HmdDriver::SensorThreadFunc, this);
    DriverLog("Sensor socket initialized. Listening on port %d", wire::kSensorPort);
    return true;
}
// Stops the sensor thread via a loopback wake packet, then closes the socket; called from Deactivate.
void HmdDriver::ShutdownSensorSocket()
{
    DriverLog("Shutting down sensor socket...");
    if (m_sensorInitialized) {
        m_sensorRunning = false;
        SOCKET wakeSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (wakeSocket != INVALID_SOCKET) {
            sockaddr_in localAddr;
            localAddr.sin_family = AF_INET;
            localAddr.sin_port = htons(wire::kSensorPort);
            inet_pton(AF_INET, "127.0.0.1", &localAddr.sin_addr);
            const char wake[] = { 0x00 };
            sendto(wakeSocket, wake, 1, 0, (sockaddr*)&localAddr, sizeof(localAddr));
            closesocket(wakeSocket);
        }
        if (m_sensorThread.joinable()) {
            m_sensorThread.join();
        }
        if (m_sensorSocket != INVALID_SOCKET) {
            closesocket(m_sensorSocket);
            m_sensorSocket = INVALID_SOCKET;
        }
        m_sensorInitialized = false;
    }
    DriverLog("Sensor socket shutdown complete.");
}
// Reads bridge-forwarded 0x10 gyro and 0x12 quaternion packets on the sensor thread; GetPose consumes the cached sample.
void HmdDriver::SensorThreadFunc()
{
    DriverLog("Sensor thread started on port %d", wire::kSensorPort);
    static constexpr int kSensorPacketLen = 45;
    uint8_t buffer[64];
    sockaddr_in senderAddr;
    int senderAddrLen = sizeof(senderAddr);
    DWORD recvTimeout = 2000;
    setsockopt(m_sensorSocket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recvTimeout, sizeof(recvTimeout));
    int logCounter = 0;
    int recvCount = 0;
    while (m_sensorRunning) {
        senderAddrLen = sizeof(senderAddr);
        int bytesReceived = recvfrom(m_sensorSocket, (char*)buffer, sizeof(buffer), 0,
                                     (sockaddr*)&senderAddr, &senderAddrLen);
        if (!m_sensorRunning) break;
        if (bytesReceived == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) {
                logCounter++;
                DebugLogThrottle(3, "Sensor: no data received after %d timeouts (hasData=%d)", logCounter, m_hasSensorData.load() ? 1 : 0);
                continue;
            }
            if (m_sensorRunning) {
                DriverLog("Sensor recvfrom error: %d", err);
            }
            continue;
        }
        DebugLogThrottle(50, "Sensor recv: %d bytes, tag=0x%02x", bytesReceived, buffer[0]);
        if (bytesReceived >= kSensorPacketLen && buffer[0] == 0x10) {
            uint64_t timestamp = 0;
            std::memcpy(&timestamp, &buffer[1], 8);
            float gyro[3], accel[3], mag[3];
            std::memcpy(gyro,  &buffer[9],  12);
            std::memcpy(accel, &buffer[21], 12);
            std::memcpy(mag,   &buffer[33], 12);
            {
                std::lock_guard<std::mutex> lock(m_sensorMutex);
                std::memcpy(m_sensorGyro, gyro, 12);
                std::memcpy(m_sensorAccel, accel, 12);
                std::memcpy(m_sensorMag, mag, 12);
                m_sensorTimestampMs = timestamp;
            }
            m_hasSensorData.store(true, std::memory_order_relaxed);
            m_lastSensorRecvMs.store(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count(),
                std::memory_order_relaxed);
            recvCount++;
            DebugLog("Sensor pkt #%d: gyro=(%.3f,%.3f,%.3f) accel=(%.1f,%.1f,%.1f) ts=%llu",
                recvCount, gyro[0], gyro[1], gyro[2], accel[0], accel[1], accel[2], (unsigned long long)timestamp);
        } else if (bytesReceived >= 25 && buffer[0] == 0x12) {
            uint64_t timestamp = 0;
            std::memcpy(&timestamp, &buffer[1], 8);
            float quat[4];
            std::memcpy(quat, &buffer[9], 16);
            {
                std::lock_guard<std::mutex> lock(m_sensorMutex);
                std::memcpy(m_sensorQuat, quat, 16);
                m_sensorTimestampMs = timestamp;
            }
            m_hasQuaternion.store(true, std::memory_order_relaxed);
            m_hasSensorData.store(true, std::memory_order_relaxed);
            m_lastSensorRecvMs.store(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count(),
                std::memory_order_relaxed);
            m_lastRotationRecvMs.store(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count(),
                std::memory_order_relaxed);
            recvCount++;
            DebugLog("Rotation pkt #%d: quat=(%.3f,%.3f,%.3f,%.3f) ts=%llu",
                recvCount, quat[0], quat[1], quat[2], quat[3], (unsigned long long)timestamp);
        } else {
            DriverLog("Sensor: got %d bytes, tag=0x%02x (not 0x10/0x12)", bytesReceived, buffer[0]);
        }
    }
    DriverLog("Sensor thread exiting (received %d packets total)", recvCount);
}
