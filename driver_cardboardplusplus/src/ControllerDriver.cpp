#include <ControllerDriver.h>

// Controller driver implementation. Exposes joystick/trackpad to SteamVR.
EVRInitError ControllerDriver::Activate(uint32_t unObjectId)
{
	// TODO: Check if every Property is okay to be recognized as a Vive controller.
	// Original example code from finallyfunctional/openvr-driver-example
	driverId = unObjectId; //unique ID for your driver

	PropertyContainerHandle_t props = VRProperties()->TrackedDeviceToPropertyContainer(driverId); //this gets a container object where you store all the information about your driver

	VRProperties()->SetStringProperty(props, Prop_RenderModelName_String, "vr_controller_vive_1_5");
	VRProperties()->SetStringProperty(props, Prop_InputProfilePath_String, "{example}/input/controller_profile.json"); //tell OpenVR where to get your driver's Input Profile
	VRProperties()->SetInt32Property(props, Prop_ControllerRoleHint_Int32, ETrackedControllerRole::TrackedControllerRole_LeftHand); //tells OpenVR what kind of device this is
	VRDriverInput()->CreateScalarComponent(props, "/input/joystick/y", &joystickYHandle, EVRScalarType::VRScalarType_Absolute,
		EVRScalarUnits::VRScalarUnits_NormalizedTwoSided); //sets up handler you'll use to send joystick commands to OpenVR with, in the Y direction (forward/backward)
	VRDriverInput()->CreateScalarComponent(props, "/input/trackpad/y", &trackpadYHandle, EVRScalarType::VRScalarType_Absolute,
		EVRScalarUnits::VRScalarUnits_NormalizedTwoSided); //sets up handler you'll use to send trackpad commands to OpenVR with, in the Y direction
	VRDriverInput()->CreateScalarComponent(props, "/input/joystick/x", &joystickXHandle, EVRScalarType::VRScalarType_Absolute,
		EVRScalarUnits::VRScalarUnits_NormalizedTwoSided); //Why VRScalarType_Absolute? Take a look at the comments on EVRScalarType.
	VRDriverInput()->CreateScalarComponent(props, "/input/trackpad/x", &trackpadXHandle, EVRScalarType::VRScalarType_Absolute,
		EVRScalarUnits::VRScalarUnits_NormalizedTwoSided); //Why VRScalarUnits_NormalizedTwoSided? Take a look at the comments on EVRScalarUnits.
	
	//The following properites are ones I tried out because I saw them in other samples, but I found they were not needed to get the sample working.
	//There are many samples, take a look at the openvr_header.h file. You can try them out.

	//VRProperties()->SetUint64Property(props, Prop_CurrentUniverseId_Uint64, 2);
	//VRProperties()->SetBoolProperty(props, Prop_HasControllerComponent_Bool, true);
	//VRProperties()->SetBoolProperty(props, Prop_NeverTracked_Bool, true);
	//VRProperties()->SetInt32Property(props, Prop_Axis0Type_Int32, k_eControllerAxis_TrackPad);
	//VRProperties()->SetInt32Property(props, Prop_Axis2Type_Int32, k_eControllerAxis_Joystick);
	//VRProperties()->SetStringProperty(props, Prop_SerialNumber_String, "example_controler_serial");
	//uint64_t availableButtons = ButtonMaskFromId(k_EButton_SteamVR_Touchpad) |
	//	ButtonMaskFromId(k_EButton_IndexController_JoyStick);
	//VRProperties()->SetUint64Property(props, Prop_SupportedButtons_Uint64, availableButtons);

	return VRInitError_None;
}

DriverPose_t ControllerDriver::GetPose()
{
	DriverPose_t pose = { 0 };
	// Report a valid, stationary pose by default so compositor treats this Controller as available.
	pose.poseIsValid = true;
	pose.result = TrackingResult_Running_OK;
	pose.deviceIsConnected = true;

	// Rotation in quaternion form. (Placeholder values for now, can be updated with real tracking data later)
	HmdQuaternion_t quat;
	quat.w = 1;
	quat.x = 0;
	quat.y = 0;
	quat.z = 0;

	pose.qWorldFromDriverRotation = quat;
	pose.qDriverFromHeadRotation = quat;

	// Position in meters. (Placeholder values for now, can be updated with real tracking data later)
	pose.vecPosition[0] = 0.0;
	pose.vecPosition[1] = 0.0;
	pose.vecPosition[2] = 0.0;

	return pose;
}

void ControllerDriver::RunFrame()
{
	// TODO: Replace the following code with real input data from your hardware. This is just an example of how to send input data to OpenVR.
	// Original example code from finallyfunctional/openvr-driver-example
	//Since we used VRScalarUnits_NormalizedTwoSided as the unit, the range is -1 to 1.
	VRDriverInput()->UpdateScalarComponent(joystickYHandle, 0.95f, 0); //move forward
	VRDriverInput()->UpdateScalarComponent(trackpadYHandle, 0.95f, 0); //move foward
	VRDriverInput()->UpdateScalarComponent(joystickXHandle, 0.0f, 0); //change the value to move sideways
	VRDriverInput()->UpdateScalarComponent(trackpadXHandle, 0.0f, 0); //change the value to move sideways
}

void ControllerDriver::Deactivate()
{
	// Clean up any state you need to here. This will be called when the driver is unloaded.
	driverId = k_unTrackedDeviceIndexInvalid;
}

void* ControllerDriver::GetComponent(const char* pchComponentNameAndVersion)
{
	// Return this class if OpenVR is requesting that.
	if (strcmp(IVRDriverInput_Version, pchComponentNameAndVersion) == 0)
	{
		return this;
	}
	return NULL;
}

void ControllerDriver::EnterStandby() {}

void ControllerDriver::DebugRequest(const char* pchRequest, char* pchResponseBuffer, uint32_t unResponseBufferSize) 
{
	if (unResponseBufferSize >= 1)
	{
		pchResponseBuffer[0] = 0;
	}
}