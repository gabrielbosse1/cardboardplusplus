#include "ControllerDriver.h"
#include <chrono>
#include <cmath>
#include <cstring>
using namespace vr;
// Called by SteamVR when the controller device is added; registers render model, role, and stick/trackpad scalars.
EVRInitError ControllerDriver::Activate(uint32_t unObjectId)
{
	m_driverId = unObjectId;
	PropertyContainerHandle_t props = VRProperties()->TrackedDeviceToPropertyContainer(m_driverId);
	VRProperties()->SetStringProperty(props, Prop_RenderModelName_String, "vr_controller_vive_1_5");
	VRProperties()->SetStringProperty(props, Prop_InputProfilePath_String, "{example}/input/controller_profile.json");
	VRProperties()->SetInt32Property(props, Prop_ControllerRoleHint_Int32, ETrackedControllerRole::TrackedControllerRole_LeftHand);
	VRDriverInput()->CreateScalarComponent(props, "/input/joystick/y", &m_joystickYHandle, EVRScalarType::VRScalarType_Absolute,
		EVRScalarUnits::VRScalarUnits_NormalizedTwoSided);
	VRDriverInput()->CreateScalarComponent(props, "/input/trackpad/y", &m_trackpadYHandle, EVRScalarType::VRScalarType_Absolute,
		EVRScalarUnits::VRScalarUnits_NormalizedTwoSided);
	VRDriverInput()->CreateScalarComponent(props, "/input/joystick/x", &m_joystickXHandle, EVRScalarType::VRScalarType_Absolute,
		EVRScalarUnits::VRScalarUnits_NormalizedTwoSided);
	VRDriverInput()->CreateScalarComponent(props, "/input/trackpad/x", &m_trackpadXHandle, EVRScalarType::VRScalarType_Absolute,
		EVRScalarUnits::VRScalarUnits_NormalizedTwoSided);
	return VRInitError_None;
}
// Called by SteamVR each frame to query controller pose; reports a connected device with identity rotation and gentle positional drift.
DriverPose_t ControllerDriver::GetPose()
{
	DriverPose_t pose = { 0 };
	pose.poseIsValid = true;
	pose.result = TrackingResult_Running_OK;
	pose.deviceIsConnected = true;
	static auto startTime = std::chrono::steady_clock::now();
	auto now = std::chrono::steady_clock::now();
	double elapsed = std::chrono::duration<double>(now - startTime).count();
	float bobHeight = (float)(sin(elapsed * 1.8) * 0.03);
	float swayX = (float)(sin(elapsed * 2.2) * 0.015);
	float forward = (float)(sin(elapsed * 1.3) * 0.01);
	HmdQuaternion_t quat;
	quat.w = 1;
	quat.x = 0;
	quat.y = 0;
	quat.z = 0;
	pose.qWorldFromDriverRotation = quat;
	pose.qDriverFromHeadRotation = quat;
	pose.vecPosition[0] = swayX;
	pose.vecPosition[1] = bobHeight;
	pose.vecPosition[2] = forward;
	return pose;
}
// Called by the provider each frame; pushes steady scalar values for the stick/trackpad inputs.
void ControllerDriver::RunFrame()
{
	VRDriverInput()->UpdateScalarComponent(m_joystickYHandle, 0.95f, 0);
	VRDriverInput()->UpdateScalarComponent(m_trackpadYHandle, 0.95f, 0);
	VRDriverInput()->UpdateScalarComponent(m_joystickXHandle, 0.0f, 0);
	VRDriverInput()->UpdateScalarComponent(m_trackpadXHandle, 0.0f, 0);
}
// Called by SteamVR when the device is removed; releases the tracked-device index.
void ControllerDriver::Deactivate()
{
	m_driverId = k_unTrackedDeviceIndexInvalid;
}
// Returns this as IVRDriverInput when SteamVR asks for the input component; null otherwise.
void* ControllerDriver::GetComponent(const char* pchComponentNameAndVersion)
{
	if (strcmp(IVRDriverInput_Version, pchComponentNameAndVersion) == 0)
	{
		return this;
	}
	return NULL;
}
// Standby hook (no-op); SteamVR calls it when the controller would sleep.
// Answers SteamVR debug console queries with an empty string; pchRequest selects the query.
void ControllerDriver::EnterStandby() {}
// Answers SteamVR debug console queries with an empty string; pchRequest selects the query.
void ControllerDriver::DebugRequest(const char* pchRequest, char* pchResponseBuffer, uint32_t unResponseBufferSize)
{
	if (unResponseBufferSize >= 1)
	{
		pchResponseBuffer[0] = 0;
	}
}