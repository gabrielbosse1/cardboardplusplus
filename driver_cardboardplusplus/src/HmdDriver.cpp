#include "HmdDriver.h"
#include <dxgi.h>
#include <cstdarg>
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

// Virtual HMD driver implementation. Presents as a display device to SteamVR.
EVRInitError HmdDriver::Activate(uint32_t unObjectId)
{
    // When I wrote this code, only God and I understood it.
    // Now only God understands it.
    // If you're an atheist, good luck.
    // I even managed to somehow get the error "'cannot open file 'kernel32.lib'"
    driverId = unObjectId;
    m_currentSwapSetIndex = 0;

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
        &pD3D11Device,
        &featureLevel,
        &pD3D11DeviceContext
    );

    if (FAILED(hr)) {
        DriverLog("D3D11 device creation failed! HRESULT: 0x%x", hr);
        return VRInitError_Init_Internal;
    }

    DriverLog("D3D11 device initialized successfully");

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
    VRProperties()->SetBoolProperty(props, Prop_DisplayDebugMode_Bool, true);
    VRProperties()->SetBoolProperty(props, Prop_HasDriverDirectModeComponent_Bool, true);

    DriverLog("HMD properties set: HasDriverDirectModeComponent=true, IsDisplayOnDesktop=false");

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
    // Report a valid, stationary pose by default so compositor treats this HMD as available.
    pose.poseIsValid = true;
    pose.result = TrackingResult_Running_OK;
    pose.deviceIsConnected = true;

    // identity rotations / zero translation
    HmdQuaternion_t quat;
    quat.w = 1.0;
    quat.x = 0.0;
    quat.y = 0.0;
    quat.z = 0.0;

    pose.qWorldFromDriverRotation = quat;
    pose.qDriverFromHeadRotation = quat;
    pose.vecPosition[0] = 0.0;
    pose.vecPosition[1] = 0.0;
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
    if (pnWidth) *pnWidth = 1920; // single-eye recommended width
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
    *pfLeft = -1.0;
    *pfRight = 1.0;
    *pfTop = -1.0;
    *pfBottom = 1.0;
}

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

void HmdDriver::CreateSwapTextureSet(uint32_t unPid, const SwapTextureSetDesc_t* pSwapTextureSetDesc, SwapTextureSet_t* pOutSwapTextureSet)
{
    DriverLog("CreateSwapTextureSet called: width=%d, height=%d, format=%d, samples=%d",
        pSwapTextureSetDesc->nWidth, pSwapTextureSetDesc->nHeight, pSwapTextureSetDesc->nFormat, pSwapTextureSetDesc->nSampleCount);

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = pSwapTextureSetDesc->nWidth;
    desc.Height = pSwapTextureSetDesc->nHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // Use a known good format
    desc.SampleDesc.Count = 1;  // No MSAA for now
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;  // Need keyed mutex for AcquireSync

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

    // Store for later
    SwapTextureSet sts;
    sts.pTexture = pTexture;
    sts.hSharedHandle = hSharedHandle;

    auto it = m_swapTextureSets.find(unPid);
    if (it == m_swapTextureSets.end()) {
        std::vector<SwapTextureSet> vec;
        vec.push_back(sts);
        m_swapTextureSets[unPid] = vec;
    } else {
        it->second.push_back(sts);
    }
}

void HmdDriver::DestroySwapTextureSet(vr::SharedTextureHandle_t sharedTextureHandle)
{
    DriverLog("DestroySwapTextureSet called: handle=%llu", (uint64_t)sharedTextureHandle);

    HANDLE h = (HANDLE)sharedTextureHandle;

    for (auto& pair : m_swapTextureSets) {
        for (auto it = pair.second.begin(); it != pair.second.end(); ++it) {
            if (it->hSharedHandle == h) {
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
    DriverLog("DestroyAllSwapTextureSets called for pid=%d", unPid);

    auto it = m_swapTextureSets.find(unPid);
    if (it != m_swapTextureSets.end()) {
        for (auto& sts : it->second) {
            if (sts.pTexture) {
                sts.pTexture->Release();
            }
        }
        m_swapTextureSets.erase(it);
    }
}

void HmdDriver::GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t sharedTextureHandles[2], uint32_t(*pIndices)[2])
{
    DriverLog("GetNextSwapTextureSetIndex called");
    (*pIndices)[0] = 0;
    (*pIndices)[1] = 0;
}

void HmdDriver::SubmitLayer(const SubmitLayerPerEye_t(&perEye)[2])
{
    DriverLog("SubmitLayer called - left: %llu, right: %llu",
        (uint64_t)perEye[0].hTexture, (uint64_t)perEye[1].hTexture);
}

void HmdDriver::Present(vr::SharedTextureHandle_t syncTexture)
{
    DriverLog("Present called! syncTexture=%llu", (uint64_t)syncTexture);

    // SIMPLE VERSION - just log for now, no D3D operations to avoid freeze
    // The sync texture handling can be added after we confirm Present works
}

void HmdDriver::PostPresent()
{
    DriverLog("PostPresent called");
}

void HmdDriver::GetFrameTiming(DriverDirectMode_FrameTiming* pFrameTiming)
{
    DriverLog("GetFrameTiming called");
}
