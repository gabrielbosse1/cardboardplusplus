#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d11.h>
#include "openvr_driver.h"
#include "VideoEncoder.h"
#include <map>
#include <vector>
#include <utility>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <memory>

#pragma comment(lib, "ws2_32.lib")

using namespace vr;

// A real triple-buffered swap texture set. Three distinct shared textures
// so the app, compositor, and driver can each own a buffer simultaneously.
struct SwapTextureSet {
    ID3D11Texture2D* pTextures[3];
    HANDLE hSharedHandles[3];
    uint32_t nextIndex; // round-robin index handed out by GetNextSwapTextureSetIndex
};

// Per SubmitLayer call: both eyes + each eye's valid bounds.
// SteamVR calls SubmitLayer once per layer; the driver must composite all of
// them in submission order (see FLICKER_ISSUE_MAP.md §10 — H1 fix).
struct SubmitLayerInfo {
    vr::SharedTextureHandle_t hTextureLeft;
    vr::SharedTextureHandle_t hTextureRight;
    vr::VRTextureBounds_t boundsLeft;
    vr::VRTextureBounds_t boundsRight;
};

// A frame queued for the background encoder thread.
struct PendingFrame {
    ID3D11Texture2D* pLeft;
    ID3D11Texture2D* pRight;
    int64_t pts;
    bool valid;
    // One entry per submitted layer, opened on the encoding thread's own D3D11
    // device and composited in submission order (painter's algorithm).
    struct PendingLayer {
        HANDLE hLeft = nullptr;
        HANDLE hRight = nullptr;
        vr::VRTextureBounds_t boundsLeft;
        vr::VRTextureBounds_t boundsRight;
    };
    std::vector<PendingLayer> layers;
};

/**
 * Virtual HMD device driver for SteamVR. Presents as a display to OpenVR.
 *
 * The implementation is deliberately split across one translation unit per
 * concern so each subsystem can be reviewed on its own:
 *   - HmdDriver.cpp      device lifecycle + SteamVR probe/pose/display methods
 *   - DirectMode.cpp     swap texture sets + Present/SubmitLayer compositing
 *   - EncoderSetup.cpp   encoder lifecycle + hardware-cap reconfiguration
 *   - EncodingThread.cpp the background GPU-readback + encode thread loop
 *   - UdpTransport.cpp   H264 framing + UDP streaming to the bridge
 *   - Discovery.cpp      phone broadcast discovery + cap negotiation
 *
 * Every subsystem touched below is owned by this class (raw pointers are the
 * pre-existing ownership model; they are released in Deactivate / Shutdown).
 */
class HmdDriver : public ITrackedDeviceServerDriver, public IVRDisplayComponent, public IVRDriverDirectModeComponent
{
public:
    // ITrackedDeviceServerDriver
    EVRInitError Activate(uint32_t unObjectId);
    void Deactivate();
    void EnterStandby();
    void* GetComponent(const char* pchComponentNameAndVersion);
    void DebugRequest(const char* pchRequest, char* pchResponseBuffer, uint32_t unResponseBufferSize);
    DriverPose_t GetPose();
    void RunFrame();

    // IVRDisplayComponent
    void GetWindowBounds( int32_t *pnX, int32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight );
    bool IsDisplayOnDesktop();
    bool IsDisplayRealDisplay();
    void GetRecommendedRenderTargetSize( uint32_t *pnWidth, uint32_t *pnHeight );
    void GetEyeOutputViewport( EVREye eEye, uint32_t *pnX, uint32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight );
    void GetProjectionRaw( EVREye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom );
    DistortionCoordinates_t ComputeDistortion( EVREye eEye, float fU, float fV );

    // IVRDriverDirectModeComponent
    void CreateSwapTextureSet(uint32_t unPid, const SwapTextureSetDesc_t* pSwapTextureSetDesc, SwapTextureSet_t* pOutSwapTextureSet) override;
    void DestroySwapTextureSet(vr::SharedTextureHandle_t sharedTextureHandle) override;
    void DestroyAllSwapTextureSets(uint32_t unPid) override;
    void GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t sharedTextureHandles[2], uint32_t(*pIndices)[2]) override;
    void SubmitLayer(const SubmitLayerPerEye_t(&perEye)[2]) override;
    void Present(vr::SharedTextureHandle_t syncTexture) override;
    void PostPresent() override;
    void GetFrameTiming(DriverDirectMode_FrameTiming* pFrameTiming) override;

    // Fan-out send: one framed packet to every active target (preview + phone).
    void SendFannedOut(const uint8_t* raw, int rawSize,
                       const uint8_t* framed, int framedSize);

    // Encoder surface used by the encoder subsystem (callback into this class).
    VideoEncoder* GetVideoEncoder() { return m_pVideoEncoder; }
    void OnEncodedPacket(uint8_t* data, int size, int64_t pts, bool keyframe);

private:
    // ---- encoder lifecycle / configuration ----
    bool InitializeVideoEncoder();
    void ShutdownVideoEncoder();
    bool ApplyHardwareCap(int capW, int capH);
    void ClampEncoderToCap();

    // ---- UDP transport (video streaming to the bridge) ----
    bool InitializeUDP();
    void ShutdownUDP();

    // ---- phone discovery (broadcast + cap negotiation) ----
    bool InitializeDiscovery();
    void ShutdownDiscovery();
    void DiscoveryThreadFunc();
    void SwitchDataTarget(const char* phoneIp);
    void SendBridgeStats(const sockaddr_in& addr); // periodic BRIDGE_STATS to the bridge

    // ---- background encoding loop + compositor sync-texture handshake ----
    void EncodingThreadFunc();
    void EncodePendingFrame(const PendingFrame& frame);
    bool AcquireSyncTexture(vr::SharedTextureHandle_t syncTexture);
    void ReleaseSyncTexture();
    void WaitEncoderIdle();

    // ---- device identity / shared D3D11 device ----
    uint32_t m_driverId;
    ID3D11Device* m_pD3D11Device;
    ID3D11DeviceContext* m_pD3D11DeviceContext;

    // ---- swap-texture-set bookkeeping (per process pid) ----
    std::map<uint32_t, std::vector<std::shared_ptr<SwapTextureSet>>> m_swapTextureSets;
    // Map from SharedTextureHandle to the actual D3D11 texture.
    std::map<vr::SharedTextureHandle_t, ID3D11Texture2D*> m_textureHandleMap;
    // Map from SharedTextureHandle to the owning swap texture set (for index rotation).
    std::map<vr::SharedTextureHandle_t, std::shared_ptr<SwapTextureSet>> m_setByHandle;

    // ---- cached compositor sync texture (opened once, per Valve's recommendation) ----
    HANDLE m_cachedSyncHandle;
    ID3D11Texture2D* m_pSyncTexture;
    IDXGIKeyedMutex* m_pSyncMutex;
    bool m_syncAcquired;

    // ---- background encoder thread handshake ----
    std::thread m_encodingThread;
    std::atomic<bool> m_encodingRunning;
    std::mutex m_encodeMutex;
    std::condition_variable m_encodeCv;
    bool m_frameQueued;
    PendingFrame m_pendingFrame;
    // Signaled by the encoder thread when it has finished ALL GPU/CPU work
    // for the previous frame (including readback), so Present can safely
    // queue new GPU work on the shared D3D11 context.
    std::mutex m_encodeDoneMutex;
    std::condition_variable m_encodeDoneCv;
    bool m_encodeDone;

    // ---- encoder state (mutable so it can be clamped to the phone's cap) ----
    VideoEncoder* m_pVideoEncoder;
    bool m_encoderInitialized;
    int64_t m_encoderPts;
    int m_encoderW = 2880;
    int m_encoderH = 1620;
    int m_encoderFps = 60;
    int m_encoderBitrate = 20000000;
    bool m_encoderUseGpu = false;
    int m_pendingCapW = 0;
    int m_pendingCapH = 0;
    std::mutex m_encoderMutex;

    // Private owned copies of eye textures so Present() can release the sync
    // texture immediately after a fast GPU copy, instead of doing the full
    // ComposeSBSGPU on the compositor thread.
    // One (left,right) pair per submitted layer; shared with the encoding
    // thread's second D3D11 device and composited in order there.
    struct LayerCopy {
        ID3D11Texture2D* pLeft = nullptr;
        HANDLE hLeft = nullptr;
        ID3D11Texture2D* pRight = nullptr;
        HANDLE hRight = nullptr;
        int width = 0;
        int height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    };
    std::vector<LayerCopy> m_layerCopies;
    bool EnsureLayerCopies(const std::vector<SubmitLayerInfo>& layers);
    std::atomic<bool> m_sceneTearingDown{false};  // set during DestroyAllSwapTextureSets

    // Present-rate pacing diagnostics.
    int m_presentCount = 0;
    long long m_lastPresentLogNs = 0;

    // Accumulated SubmitLayer calls since the last Present (composited together,
    // in submission order, as one SBS frame). m_hasSubmit is atomic: SubmitLayer
    // (compositor submit thread) writes it, Present (present thread) reads it.
    std::vector<SubmitLayerInfo> m_submitLayers;
    std::mutex m_submitLayersMutex;
    std::atomic<bool> m_hasSubmit{false};

    // ---- UDP socket (video stream to the bridge) ----
    SOCKET m_udpSocket;
    sockaddr_in m_serverAddr;    // phone target; set by SwitchDataTarget on discovery
    sockaddr_in m_previewAddr;   // permanent localhost preview target (127.0.0.1:42069)
    std::atomic<bool> m_hasPhoneTarget{false};  // m_serverAddr holds a real phone
    std::atomic<bool> m_previewEnabled{true};   // localhost preview send (BRIDGE_PREVIEW)
    bool m_udpInitialized;
    uint32_t m_udpDroppedFrames;
    std::atomic<uint64_t> m_udpFramesSent{0};   // total framed packets actually sent

    // ---- UDP discovery socket (phone broadcast) ----
    SOCKET m_discoverySocket;
    bool m_discoveryInitialized;
    std::atomic<bool> m_discoveryRunning;
    std::thread m_discoveryThread;
    std::mutex m_targetIpMutex;
    std::atomic<long long> m_lastPhonePacketMs{0};  // last time a phone packet was received (GetTickCount64)
};