#pragma once
#include "openvr_driver.h"
#include <windows.h>
#include <atomic>
#include <cstdint>
using namespace vr;
// Forward declaration; full definition lives in HmdDriver.h.
class HmdDriver;
// Virtual hand-tracker device: receives skeleton bone data from the bridge
// (via HmdDriver::GetSkeletonHand) and publishes it to SteamVR as a
// skeletal-input controller. Created/destroyed dynamically by DeviceProvider
// when hands appear/disappear.
class HandDriver : public ITrackedDeviceServerDriver
{
public:
    HandDriver(HmdDriver* hmd, int handId);
    // SteamVR device lifecycle
    EVRInitError Activate(uint32_t unObjectId);
    void Deactivate();
    void EnterStandby();
    void* GetComponent(const char* pchComponentNameAndVersion);
    void DebugRequest(const char* pchRequest, char* pchResponseBuffer, uint32_t unResponseBufferSize);
    DriverPose_t GetPose();
    // Called by DeviceProvider::RunFrame each frame to push latest skeleton
    void RunFrame();
    // True when this device is registered with SteamVR
    bool IsActive() const { return m_active; }
    int HandId() const { return m_handId; }
private:
    HmdDriver* m_hmd;
    int m_handId; // 0=left, 1=right
    uint32_t m_deviceId = k_unTrackedDeviceIndexInvalid;
    VRInputComponentHandle_t m_skeletalHandle{0};
    std::atomic<bool> m_active{false};
    DriverPose_t m_pose = {};
    // Bone cache for the current frame
    static constexpr int BONE_COUNT = 31;
    VRBoneTransform_t m_bones[BONE_COUNT]{};
};
