#pragma once
#include "HmdDriver.h"
#include "openvr_driver.h"
#include <windows.h>
using namespace vr;
// SteamVR device provider; owns the single HmdDriver and forwards Init/RunFrame/Cleanup.
class DeviceProvider : public IServerTrackedDeviceProvider
{
public:
// SteamVR provider hooks; Init/Cleanup manage the HMD lifetime, RunFrame drives the per-frame pose publish.
	EVRInitError Init(IVRDriverContext* pDriverContext);
	void Cleanup();
	const char* const* GetInterfaceVersions();
	void RunFrame();
	bool ShouldBlockStandbyMode();
	void EnterStandby();
	void LeaveStandby();
private:
	HmdDriver* m_hmdDriver;
};