#pragma once
// Minimal Linux-only declarations for SteamVR's IPC resource manager. The
// vendored OpenVR header predates this interface; the vtable order and struct
// layout below match Valve's current openvr_driver.h byte for byte.
#ifndef _WIN32
#include <cstdint>

namespace vr
{
static const uint32_t MaxDmabufPlaneCount = 4;

struct DmabufPlane_t
{
    uint32_t unOffset;
    uint32_t unStride;
    int32_t nFd; // SteamVR dups this fd; the caller keeps ownership of its own.
};

struct DmabufAttributes_t
{
    void* pNext;
    uint32_t unWidth;
    uint32_t unHeight;
    uint32_t unDepth;
    uint32_t unMipLevels;
    uint32_t unArrayLayers;
    uint32_t unSampleCount;
    uint32_t unFormat;
    uint64_t ulModifier;
    uint32_t unPlaneCount;
    DmabufPlane_t plane[MaxDmabufPlaneCount];
};

class IVRIPCResourceManagerClient
{
public:
    virtual bool NewSharedVulkanImage(uint32_t, uint32_t, uint32_t, bool, bool, bool,
                                      uint32_t, uint32_t, uint32_t, uint32_t,
                                      SharedTextureHandle_t*) = 0;
    virtual bool NewSharedVulkanBuffer(uint32_t, uint32_t, SharedTextureHandle_t*) = 0;
    virtual bool NewSharedVulkanSemaphore(bool, SharedTextureHandle_t*) = 0;
    virtual bool RefResource(SharedTextureHandle_t, uint64_t*) = 0;
    virtual bool UnrefResource(SharedTextureHandle_t) = 0;
    virtual bool GetDmabufFormats(uint32_t*, uint32_t*) = 0;
    virtual bool GetDmabufModifiers(EVRApplicationType, uint32_t, uint32_t*, uint64_t*) = 0;
    virtual bool ImportDmabuf(EVRApplicationType, DmabufAttributes_t*, SharedTextureHandle_t*) = 0;
    virtual bool ReceiveSharedFd(uint64_t, int*) = 0;
protected:
    virtual ~IVRIPCResourceManagerClient() = default;
};
static const char* IVRIPCResourceManagerClient_Version = "IVRIPCResourceManagerClient_003";

inline IVRIPCResourceManagerClient* IPCResourceManager() {
    vr::EVRInitError err = vr::VRInitError_None;
    void* p = vr::VRDriverContext()->GetGenericInterface(IVRIPCResourceManagerClient_Version, &err);
    return static_cast<IVRIPCResourceManagerClient*>(p);
}
} // namespace vr

// DRM fourcc helpers for the swap-chain formats we support.
#define CBPP_DRM_FORMAT(c0, c1, c2, c3) \
    ((uint32_t)(c0) | ((uint32_t)(c1) << 8) | ((uint32_t)(c2) << 16) | ((uint32_t)(c3) << 24))
#define CBPP_DRM_FORMAT_ARGB8888 CBPP_DRM_FORMAT('A', 'R', '2', '4') // B8G8R8A8 in memory
#define CBPP_DRM_FORMAT_ABGR8888 CBPP_DRM_FORMAT('A', 'B', '2', '4') // R8G8B8A8 in memory
#define CBPP_DRM_FORMAT_MOD_INVALID 0x00FFFFFFFFFFFFFFULL
#endif
