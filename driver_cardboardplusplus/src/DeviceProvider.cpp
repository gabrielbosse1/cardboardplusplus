#include "DeviceProvider.h"
#include "ControllerDriver.h"
#include "HmdDriver.h"
#include "openvr_driver.h"

using namespace vr;

// Device provider entry point. Registers controller and HMD drivers with SteamVR.
EVRInitError DeviceProvider::Init(IVRDriverContext* pDriverContext)
{
    EVRInitError initError = InitServerDriverContext(pDriverContext);
    if (initError != EVRInitError::VRInitError_None)
    {
        return initError;
    }
    
    VRDriverLog()->Log("Initializing example controller"); //this is how you log out Steam's log file.

    m_controllerDriver = new ControllerDriver();
    VRServerDriverHost()->TrackedDeviceAdded("example_controller", TrackedDeviceClass_Controller, m_controllerDriver); //add all your devices like this.

    VRDriverLog()->Log("Initializing example virtual HMD");
    m_hmdDriver = new HmdDriver();
    VRServerDriverHost()->TrackedDeviceAdded("example_virtual_hmd", TrackedDeviceClass_HMD, m_hmdDriver);

    return vr::VRInitError_None;
}

void DeviceProvider::Cleanup()
{
    delete m_controllerDriver;
    m_controllerDriver = NULL;
    delete m_hmdDriver;
    m_hmdDriver = NULL;
}
const char* const* DeviceProvider::GetInterfaceVersions()
{
    return k_InterfaceVersions;
}

void DeviceProvider::RunFrame()
{
    m_controllerDriver->RunFrame();
    m_hmdDriver->RunFrame();
}

bool DeviceProvider::ShouldBlockStandbyMode()
{
    return false;
}

void DeviceProvider::EnterStandby() {}

void DeviceProvider::LeaveStandby() {}