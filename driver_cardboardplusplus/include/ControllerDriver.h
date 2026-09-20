#pragma once
#include "openvr_driver.h"
#include <windows.h>
using namespace vr;
// Placeholder SteamVR controller device; exposes stick/trackpad scalars, not driven by phone input.
class ControllerDriver : public ITrackedDeviceServerDriver
{
public:
// SteamVR device lifetime + per-frame hooks; Activate/GetComponent/DebugRequest serve vrserver, RunFrame/GetPose feed input.
	EVRInitError Activate(uint32_t unObjectId);
	void Deactivate();
	void EnterStandby();
	void* GetComponent(const char* pchComponentNameAndVersion);
	void DebugRequest(const char* pchRequest, char* pchResponseBuffer, uint32_t unResponseBufferSize);
	DriverPose_t GetPose();
	void RunFrame();
private:
// SteamVR-assigned device index plus input component handles written each RunFrame.
	uint32_t m_driverId;
	VRInputComponentHandle_t m_joystickYHandle;
	VRInputComponentHandle_t m_trackpadYHandle;
	VRInputComponentHandle_t m_joystickXHandle;
	VRInputComponentHandle_t m_trackpadXHandle;
};