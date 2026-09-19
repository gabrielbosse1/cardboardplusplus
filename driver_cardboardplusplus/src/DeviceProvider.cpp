#include "DeviceProvider.h"
#include "ControllerDriver.h"
#include "HmdDriver.h"
#include "openvr_driver.h"

using namespace vr;

// Device provider entry point. Registers the HMD driver with SteamVR.
//
// The example controller from the OpenVR template is intentionally NOT
// registered (M1): it drove a hardcoded joystick forward input plus a sine
// bob with no real hardware behind it. Hands ship via the bridge hand
// pipeline instead, never as a ghost device.
EVRInitError DeviceProvider::Init(IVRDriverContext* pDriverContext)
{
    EVRInitError initError = InitServerDriverContext(pDriverContext);
    if (initError != EVRInitError::VRInitError_None)
    {
        return initError;
    }
    
    VRDriverLog()->Log("Initializing cardboardplusplus virtual HMD");
    m_hmdDriver = new HmdDriver();
    VRServerDriverHost()->TrackedDeviceAdded("cardboardplusplus_hmd", TrackedDeviceClass_HMD, m_hmdDriver);

    return vr::VRInitError_None;
}

void DeviceProvider::Cleanup()
{
    // m_controllerDriver is never created (ghost controller not registered).
    delete m_hmdDriver;
    m_hmdDriver = NULL;
}
const char* const* DeviceProvider::GetInterfaceVersions()
{
    return k_InterfaceVersions;
}

void DeviceProvider::RunFrame()
{
    static int count = 0;
    count++;
    if (count == 1) VRDriverLog()->Log("DeviceProvider::RunFrame FIRST CALL");
    m_hmdDriver->RunFrame();
}

bool DeviceProvider::ShouldBlockStandbyMode()
{
    return true;
}

void DeviceProvider::EnterStandby() {}

void DeviceProvider::LeaveStandby() {}