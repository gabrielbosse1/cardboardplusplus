#include "DeviceProvider.h"
#include "ControllerDriver.h"
#include "HmdDriver.h"
#include "HandDriver.h"
#include "DriverLog.h"
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
// Called by SteamVR at driver unload; destroys all devices created in Init.
void DeviceProvider::Cleanup()
{
    for (int i = 0; i < 2; i++) {
        if (m_handDevices[i]) {
            delete m_handDevices[i];
            m_handDevices[i] = nullptr;
            m_handRegistered[i] = false;
        }
    }
    delete m_hmdDriver;
    m_hmdDriver = NULL;
}
// Returns the null-terminated interface version list SteamVR queries after HmdDriverFactory.
const char* const* DeviceProvider::GetInterfaceVersions()
{
    return k_InterfaceVersions;
}
// Called by SteamVR each frame; drives the HMD pose and dynamically spawns/removes hand devices.
void DeviceProvider::RunFrame()
{
    static int count = 0;
    count++;
    if (count == 1) VRDriverLog()->Log("DeviceProvider::RunFrame FIRST CALL");
    m_hmdDriver->RunFrame();
    EnsureHandDevices();
    for (int i = 0; i < 2; i++) {
        if (m_handDevices[i] && m_handRegistered[i]) {
            m_handDevices[i]->RunFrame();
        }
    }
}
// Spawns hand devices when skeleton data first arrives and keeps them
// registered; HandDriver reports poseIsValid=false while stale.
void DeviceProvider::EnsureHandDevices()
{
    static const char* serials[2] = { "CBPP_HAND_LEFT", "CBPP_HAND_RIGHT" };
    for (int i = 0; i < 2; i++) {
        HmdDriver::SkeletonHand snap;
        bool hasData = m_hmdDriver->GetSkeletonHand(i, snap);
        if (hasData && !m_handRegistered[i]) {
            // Spawn the hand device
            if (!m_handDevices[i]) {
                m_handDevices[i] = new HandDriver(m_hmdDriver, i);
            }
            VRServerDriverHost()->TrackedDeviceAdded(
                serials[i], TrackedDeviceClass_Controller, m_handDevices[i]);
            m_handRegistered[i] = true;
            DriverLog("Hand device spawned: %s", serials[i]);
        }
    }
}
// Tells SteamVR standby must stay disabled so sockets and encoder threads keep running.
bool DeviceProvider::ShouldBlockStandbyMode()
{
    return true;
}
void DeviceProvider::EnterStandby() {}
void DeviceProvider::LeaveStandby() {}