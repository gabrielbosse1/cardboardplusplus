#pragma once
#include "Platform.h"
#ifdef _WIN32
#include <d3d11.h>
#endif
#include "openvr_driver.h"
#include "GpuImage.h"
#include "VideoEncoder.h"
#include "VulkanContext.h"
#include "BridgeServer.h"
#include <map>
#include <vector>
#include <utility>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <memory>
using namespace vr;
// Triple-buffered eye textures shared with the SteamVR compositor; nextIndex rotates the write slot.
struct SwapTextureSet {
#ifdef _WIN32
    ID3D11Texture2D* images[3] = {};
    HANDLE sharedHandles[3] = {};
#else
    // Linux exports each image as a dma-buf fd; the compositor imports the memory
    // with VK_KHR_external_memory_fd and hands the fd back through SubmitLayer.
    VkImage images[3] = {};
    VkDeviceMemory memory[3] = {};
    int fds[3] = {};
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
#endif
    uint32_t nextIndex = 0;
};
// One queued compositor layer with per-eye texture handles and UV bounds; filled by SubmitLayer, drained by Present.
struct SubmitLayerInfo {
    vr::SharedTextureHandle_t hTextureLeft;
    vr::SharedTextureHandle_t hTextureRight;
    vr::VRTextureBounds_t boundsLeft;
    vr::VRTextureBounds_t boundsRight;
};
// Frame handed from the Present thread to the encoding thread.
struct PendingFrame {
    int64_t pts;
    bool valid;
    struct PendingLayer {
        GpuImage left;
        GpuImage right;
        vr::VRTextureBounds_t boundsLeft;
        vr::VRTextureBounds_t boundsRight;
    };
    std::vector<PendingLayer> layers;
};
// Virtual HMD: serves SteamVR display/direct-mode interfaces, composites eye layers, encodes H.264, streams over UDP.
class HmdDriver : public ITrackedDeviceServerDriver, public IVRDisplayComponent, public IVRDriverDirectModeComponent
{
public:
// SteamVR device + display + direct-mode hooks; Activate/Deactivate bracket the session, RunFrame/GetPose publish tracking.
    EVRInitError Activate(uint32_t unObjectId);
    void Deactivate();
    void EnterStandby();
    void* GetComponent(const char* pchComponentNameAndVersion);
    void DebugRequest(const char* pchRequest, char* pchResponseBuffer, uint32_t unResponseBufferSize);
    DriverPose_t GetPose();
    void RunFrame();
    void GetWindowBounds( int32_t *pnX, int32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight );
    bool IsDisplayOnDesktop();
    bool IsDisplayRealDisplay();
    void GetRecommendedRenderTargetSize( uint32_t *pnWidth, uint32_t *pnHeight );
    void GetEyeOutputViewport( EVREye eEye, uint32_t *pnX, uint32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight );
    void GetProjectionRaw( EVREye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom );
    DistortionCoordinates_t ComputeDistortion( EVREye eEye, float fU, float fV );
    void CreateSwapTextureSet(uint32_t unPid, const SwapTextureSetDesc_t* pSwapTextureSetDesc, SwapTextureSet_t* pOutSwapTextureSet) override;
    void DestroySwapTextureSet(vr::SharedTextureHandle_t sharedTextureHandle) override;
    void DestroyAllSwapTextureSets(uint32_t unPid) override;
    void GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t sharedTextureHandles[2], uint32_t(*pIndices)[2]) override;
    void SubmitLayer(const SubmitLayerPerEye_t(&perEye)[2]) override;
    void Present(vr::SharedTextureHandle_t syncTexture) override;
    void PostPresent() override;
    void GetFrameTiming(DriverDirectMode_FrameTiming* pFrameTiming) override;
// Encoded-packet fan-out to preview + phone; GetVideoEncoder exposes the encoder, OnEncodedPacket receives each access unit.
    void SendFannedOut(const uint8_t* raw, int rawSize,
                       const uint8_t* framed, int framedSize);
    VideoEncoder* GetVideoEncoder() { return m_pVideoEncoder; }
    void OnEncodedPacket(uint8_t* data, int size, int64_t pts, bool keyframe);
private:
    // Encoder + bridge + socket lifecycle helpers; each Initialize/Shutdown pair owns one subsystem thread or object.
    bool InitializeVideoEncoder();
    void ShutdownVideoEncoder();
    bool ApplyHardwareCap(int capW, int capH);
    void ClampEncoderToCap();
    bool ApplyBridgeCfg(int fps, int bitrateKbps, const char* codec);
    void ApplyStreamSettings(const cbpp::PayloadSettingsChange& settings);
    bool ApplyEncoderSettings(int w, int h, int fps, int bitrateBps, bool useGpu, const char* reason);
    static bool SanitizeEncoderDims(int& w, int& h);
    void RegisterEncoderCallbacks(VideoEncoder* enc);
    void RunBridgeHeartbeat();
    bool InitializeBridge();
    void ShutdownBridge();
    bool InitializeUDP();
    void ShutdownUDP();
    bool InitializeDiscovery();
    void ShutdownDiscovery();
    void DiscoveryThreadFunc();
    void SwitchDataTarget(const char* phoneIp);
    void SendBridgeStats(const sockaddr_in& addr);
    bool InitializeSensorSocket();
    void ShutdownSensorSocket();
    void SensorThreadFunc();
// Frame pipeline helpers; the encoding thread runs EncodingThreadFunc/EncodePendingFrame, Present uses the sync/copy helpers.
    void EncodingThreadFunc();
    void EncodePendingFrame(const PendingFrame& frame);
    bool AcquireSyncTexture(vr::SharedTextureHandle_t syncTexture);
    void ReleaseSyncTexture();
    void WaitEncoderIdle();
// Device, swap-set, and sync-texture state; maps connect compositor handles to the textures owned here.
    uint32_t m_driverId;
#ifdef _WIN32
    ID3D11Device* m_pHostDevice;
    ID3D11DeviceContext* m_pHostContext;
#endif
    VulkanContext m_vulkan;
    bool InitializeVulkan();
    std::map<uint32_t, std::vector<std::shared_ptr<SwapTextureSet>>> m_swapTextureSets;
    std::map<vr::SharedTextureHandle_t, std::shared_ptr<SwapTextureSet>> m_setByHandle;
    uint64_t m_cachedSyncHandle = 0;
    bool m_syncAcquired = false;
#ifdef _WIN32
    std::map<vr::SharedTextureHandle_t, ID3D11Texture2D*> m_textureHandleMap;
    ID3D11Texture2D* m_pSyncTexture;
    IDXGIKeyedMutex* m_pSyncMutex;
#else
    // Compositor sync arrives as an external semaphore fd; it is imported once per
    // handle and waited on in Present.
    VkSemaphore m_syncSemaphore = VK_NULL_HANDLE;
#endif
// Encode queue + encoder config; Present produces PendingFrame, the encoding thread consumes it under m_encoderMutex.
    std::thread m_encodingThread;
    std::atomic<bool> m_encodingRunning;
    std::mutex m_encodeMutex;
    std::condition_variable m_encodeCv;
    bool m_frameQueued;
    PendingFrame m_pendingFrame;
    std::mutex m_encodeDoneMutex;
    std::condition_variable m_encodeDoneCv;
    bool m_encodeDone;
    VideoEncoder* m_pVideoEncoder;
    bool m_encoderInitialized;
    int64_t m_encoderPts;
    int m_encoderW = 1920;
    int m_encoderH = 1080;
    int m_encoderFps = 60;
    int m_encoderBitrate = 20000000;
    bool m_encoderUseGpu = false;
    int m_pendingCapW = 0;
    int m_pendingCapH = 0;
    std::mutex m_encoderMutex;
// Private per-eye copies shared with the encoder thread; rebuilt when layer size or format changes.
    struct LayerCopy {
        ID3D11Texture2D* pLeft = nullptr;
        HANDLE hLeft = nullptr;
        ID3D11Texture2D* pRight = nullptr;
        HANDLE hRight = nullptr;
        IDXGIKeyedMutex* syncLeft = nullptr;
        IDXGIKeyedMutex* syncRight = nullptr;
        bool leftSyncHeld = false;
        bool rightSyncHeld = false;
        int width = 0;
        int height = 0;
        uint32_t format = 0;
    };
    std::vector<LayerCopy> m_layerCopies;
    bool EnsureLayerCopies(const std::vector<SubmitLayerInfo>& layers);
    std::atomic<bool> m_sceneTearingDown{false};
    std::atomic<int> m_streamEnabled{ 1 };
    long long m_lastHeartbeatNs = 0;
    int m_presentCount = 0;
    long long m_lastPresentLogNs = 0;
// Submit queue, UDP targets, discovery/sensor sockets, sensor cache, and bridge endpoint; guarded by the matching mutex/atomic.
    std::vector<SubmitLayerInfo> m_submitLayers;
    std::mutex m_submitLayersMutex;
    std::atomic<bool> m_hasSubmit{false};
    cbpp::PlatformSocket m_udpSocket = cbpp::kInvalidSocket;
    sockaddr_in m_serverAddr;
    sockaddr_in m_previewAddr;
    std::atomic<bool> m_hasPhoneTarget{false};
    std::atomic<bool> m_previewEnabled{true};
    bool m_udpInitialized;
    uint32_t m_udpDroppedPreview;
    uint32_t m_udpDroppedPhone;
    std::atomic<uint64_t> m_udpFramesSent{0};
    std::vector<uint8_t> m_scratchFixed;
    std::vector<uint8_t> m_scratchFramed;
    cbpp::PlatformSocket m_discoverySocket = cbpp::kInvalidSocket;
    bool m_discoveryInitialized;
    std::atomic<bool> m_discoveryRunning;
    std::thread m_discoveryThread;
    std::mutex m_targetIpMutex;
    std::atomic<long long> m_lastPhonePacketMs{0};
    cbpp::PlatformSocket m_sensorSocket = cbpp::kInvalidSocket;
    bool m_sensorInitialized;
    std::atomic<bool> m_sensorRunning;
    std::thread m_sensorThread;
    std::mutex m_sensorMutex;
    float m_sensorGyro[3]{0, 0, 0};
    float m_sensorAccel[3]{0, 0, 0};
    float m_sensorMag[3]{0, 0, 0};
    uint64_t m_sensorTimestampMs{0};
    std::atomic<bool> m_hasSensorData{false};
    float m_sensorQuat[4]{1, 0, 0, 0};
    std::atomic<bool> m_hasQuaternion{false};
    std::atomic<int64_t> m_lastSensorRecvMs{0};
    std::atomic<int64_t> m_lastRotationRecvMs{0};
    vr::VRInputComponentHandle_t m_proximityHandle{0};
    cbpp::BridgeServer m_bridgeServer;
    std::atomic<bool> m_bridgeInitialized;

public:
    // Skeleton data from bridge (tag 0x13 on sensor port 42074).
    static constexpr int SKELETON_BONE_COUNT = 31;
    struct SkeletonBone { float pos[3]; float rot[4]; };
    struct SkeletonHand {
        SkeletonBone bones[SKELETON_BONE_COUNT];
        int64_t timestamp_ms{0};
        // Sender timestamp of the last accepted packet (bridge clock); only
        // compared against newer arrivals, never against timestamp_ms above.
        uint64_t packet_ts{0};
        bool connected{false};
    };

private:
    std::mutex m_skeletonMutex;
    SkeletonHand m_skeletonHands[2]{}; // 0=left, 1=right

public:
    /// Returns a snapshot of the skeleton hand data; called by HandDriver.
    bool GetSkeletonHand(int handId, SkeletonHand& out);
};
