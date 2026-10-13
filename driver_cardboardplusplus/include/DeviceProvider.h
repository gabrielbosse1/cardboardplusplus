#pragma once
#include "HmdDriver.h"
#include "HandDriver.h"
#include "openvr_driver.h"
#include <windows.h>
using namespace vr;
// SteamVR device provider; owns the HmdDriver and spawns the hand devices when the
// bridge first reports hand tracking data. The hands are registered once and live
// until Cleanup: HandDriver reports a disconnected pose while no data arrives.
class DeviceProvider : public IServerTrackedDeviceProvider
{
public:
    EVRInitError Init(IVRDriverContext* pDriverContext);
    void Cleanup();
    const char* const* GetInterfaceVersions();
    void RunFrame();
    bool ShouldBlockStandbyMode();
    void EnterStandby();
    void LeaveStandby();
private:
    HmdDriver* m_hmdDriver = nullptr;
    HandDriver* m_handDevices[2] = {}; // 0=left, 1=right
    bool m_handRegistered[2] = {};

    void EnsureHandDevices();
    void RemoveHandDevices();
};