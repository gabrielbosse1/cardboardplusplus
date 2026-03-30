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
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include "Protocol.h"

#pragma comment(lib, "ws2_32.lib")

using namespace vr;

struct SwapTextureSet {
    ID3D11Texture2D* pTexture;
    HANDLE hSharedHandle;
    IDXGIKeyedMutex* pKeyedMutex;
};

// Per-eye submit layer info from SteamVR
struct SubmitLayerInfo {
    vr::SharedTextureHandle_t hTexture;
};

// Remote client state (the phone)
struct RemoteClient {
    sockaddr_in addr;
    bool connected;
    std::string name;
    RemoteClient() : addr{}, connected(false) {}
};

/** Virtual HMD device driver for SteamVR. Presents as a display to OpenVR. */
class HmdDriver : public ITrackedDeviceServerDriver, public IVRDisplayComponent, public IVRDriverDirectModeComponent
{
public:
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

    VideoEncoder* GetVideoEncoder() { return m_pVideoEncoder; }
    void OnEncodedPacket(uint8_t* data, int size, int64_t pts, bool keyframe);

private:
    bool InitializeVideoEncoder();
    void ShutdownVideoEncoder();

    // ---- Network initialization / shutdown ----
    bool InitializeNetworking();
    void ShutdownNetworking();

    // ---- Broadcast discovery (PC -> LAN, broadcast port) ----
    bool StartBroadcastThread();
    void BroadcastLoop();

    // ---- Discovery response listener (phone -> PC, broadcast port) ----
    bool StartDiscoveryListener();
    void DiscoveryListenerLoop();

    // ---- Video socket (PC -> phone, video port) ----
    bool InitializeVideoSocket();
    void ShutdownVideoSocket();

    // ---- Tracking receiver (phone -> PC, tracking port) ----
    bool StartTrackingReceiver();
    void TrackingReceiverLoop();

    // ---- Camera receiver (phone -> PC, camera port) ----
    bool StartCameraReceiver();
    void CameraReceiverLoop();

    // ---- Helpers ----
    std::string GetLocalIPAddress();

    uint32_t driverId;

    ID3D11Device* pD3D11Device;
    ID3D11DeviceContext* pD3D11DeviceContext;

    std::map<uint32_t, std::vector<SwapTextureSet>> m_swapTextureSets;
    // Map from SharedTextureHandle to the actual D3D11 texture
    std::map<vr::SharedTextureHandle_t, ID3D11Texture2D*> m_textureHandleMap;
    // Map from SharedTextureHandle to the keyed mutex for synchronization
    std::map<vr::SharedTextureHandle_t, IDXGIKeyedMutex*> m_mutexHandleMap;
    uint32_t m_currentSwapSetIndex;

    VideoEncoder* m_pVideoEncoder;
    bool m_encoderInitialized;
    int64_t m_encoderPts;
    uint32_t m_lastEncodedPid;

    // Per-eye submit layer tracking for SBS compositing
    SubmitLayerInfo m_submitLayers[2];
    bool m_hasSubmit;

    // ---- Networking state ----
    WSADATA m_wsaData;
    bool m_wsaInitialized;

    // Broadcast discovery thread
    SOCKET m_broadcastSocket;
    std::thread m_broadcastThread;
    std::atomic<bool> m_broadcastRunning;

    // Discovery response listener thread
    SOCKET m_discoverySocket;
    std::thread m_discoveryThread;
    std::atomic<bool> m_discoveryRunning;

    // Video socket (send encoded frames to phone)
    SOCKET m_videoSocket;
    sockaddr_in m_videoTargetAddr;
    bool m_videoTargetSet;

    // Tracking receiver thread
    SOCKET m_trackingSocket;
    std::thread m_trackingThread;
    std::atomic<bool> m_trackingRunning;
    std::mutex m_trackingMutex;
    // Latest tracking data from phone
    cbpp::TrackingPayload m_latestTracking;
    bool m_hasTracking;

    // Camera receiver thread
    SOCKET m_cameraSocket;
    std::thread m_cameraThread;
    std::atomic<bool> m_cameraRunning;

    // Remote client
    RemoteClient m_client;
    std::mutex m_clientMutex;
};
