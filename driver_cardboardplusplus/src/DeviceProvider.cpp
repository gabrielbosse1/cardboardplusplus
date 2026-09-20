#include "DeviceProvider.h"
#include "ControllerDriver.h"
#include "HmdDriver.h"
#include "openvr_driver.h"
using namespace vr;
// Called by SteamVR at driver load; binds the driver context and registers the HMD device.
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
// Called by SteamVR at driver unload; destroys the HMD object created in Init.
void DeviceProvider::Cleanup()
{
    delete m_hmdDriver;
    m_hmdDriver = NULL;
}
// Returns the null-terminated interface version list SteamVR queries after HmdDriverFactory.
const char* const* DeviceProvider::GetInterfaceVersions()
{
    return k_InterfaceVersions;
}
// Called by SteamVR each frame; forwards to HmdDriver::RunFrame for pose publish + bridge heartbeat.
void DeviceProvider::RunFrame()
{
    static int count = 0;
    count++;
    if (count == 1) VRDriverLog()->Log("DeviceProvider::RunFrame FIRST CALL");
    m_hmdDriver->RunFrame();
}
// Tells SteamVR standby must stay disabled so sockets and encoder threads keep running.
bool DeviceProvider::ShouldBlockStandbyMode()
{
    return true;
}
// Standby enter hook (no-op); SteamVR calls it when the headset would sleep.
// Standby leave hook (no-op); SteamVR calls it when the headset would wake.
void DeviceProvider::EnterStandby() {}
// Standby leave hook (no-op); paired with EnterStandby above.
void DeviceProvider::LeaveStandby() {}