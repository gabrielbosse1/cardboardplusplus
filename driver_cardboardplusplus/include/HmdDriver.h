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
#include "BridgeConnection.h"
#include <map>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

#pragma comment(lib, "ws2_32.lib")

using namespace vr;

struct SwapTextureSet {
    ID3D11Texture2D* pTexture;
    HANDLE hSharedHandle;
    IDXGIKeyedMutex* pKeyedMutex;
};

struct SubmitLayerInfo {
    vr::SharedTextureHandle_t hTexture;
};

struct PendingEncode {
    ID3D11Texture2D* pLeftTex = nullptr;
    ID3D11Texture2D* pRightTex = nullptr;
    int64_t pts = 0;
    IDXGIKeyedMutex* pSyncMutex = nullptr;
    ID3D11Texture2D* pSyncTexture = nullptr;
    HANDLE hSyncHandle = nullptr;
};

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

    VideoEncoder* GetVideoEncoder() { return m_pVideoEncoder; }
    void OnEncodedPacket(uint8_t* data, int size, int64_t pts, bool keyframe);

    void SetBridgeConnection(BridgeConnection* conn) { m_bridgeConnection = conn; }

private:
    bool InitializeVideoEncoder();
    void ShutdownVideoEncoder();
    bool ApplyHardwareCap(int capW, int capH);
    void ClampEncoderToCap();

    bool InitializeUDP();
    void ShutdownUDP();

    bool InitializeDiscovery();
    void ShutdownDiscovery();
    void DiscoveryThreadFunc();
    void SwitchDataTarget(const char* phoneIp);

    uint32_t driverId;

    ID3D11Device* pD3D11Device;
    ID3D11DeviceContext* pD3D11DeviceContext;

    std::map<uint32_t, std::vector<SwapTextureSet>> m_swapTextureSets;
    std::map<vr::SharedTextureHandle_t, ID3D11Texture2D*> m_textureHandleMap;
    std::map<vr::SharedTextureHandle_t, IDXGIKeyedMutex*> m_mutexHandleMap;
    uint32_t m_currentSwapSetIndex;

    VideoEncoder* m_pVideoEncoder;
    bool m_encoderInitialized;
    int64_t m_encoderPts;
    uint32_t m_lastEncodedPid;

    int m_encoderW = 2880;
    int m_encoderH = 1620;
    int m_encoderFps = 60;
    int m_encoderBitrate = 20000000;

    int m_pendingCapW = 0;
    int m_pendingCapH = 0;
    std::mutex m_encoderMutex;

    PendingEncode m_pendingEncode;
    bool m_hasPendingEncode = false;
    int m_presentCount = 0;
    long long m_lastPresentLogNs = 0;

    SubmitLayerInfo m_submitLayers[2];
    bool m_hasSubmit;

    SOCKET m_udpSocket;
    sockaddr_in m_serverAddr;
    bool m_udpInitialized;

    SOCKET m_discoverySocket;
    bool m_discoveryInitialized;
    std::atomic<bool> m_discoveryRunning;
    std::thread m_discoveryThread;
    std::mutex m_targetIpMutex;

    BridgeConnection* m_bridgeConnection = nullptr;

    // Measured encoder FPS (actual frames encoded per second)
    std::atomic<float> m_measuredEncoderFps{0.0f};
    int m_encodeFrameCount = 0;
    long long m_lastEncodeLogNs = 0;

    // Measured present FPS (compositor presentation rate)
    std::atomic<float> m_measuredPresentFps{0.0f};

    // Background encoding thread
    void EncodingThreadFunc();
    std::thread m_encodingThread;
    std::mutex m_encodingMutex;
    std::condition_variable m_encodingCv;
    std::atomic<bool> m_encodingThreadRunning{false};
    std::atomic<bool> m_encodingWorkReady{false};
};
