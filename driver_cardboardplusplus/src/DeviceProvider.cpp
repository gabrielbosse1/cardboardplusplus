#include "DeviceProvider.h"
#include "HmdDriver.h"
#include "HandDriver.h"
#include "DriverLog.h"
#include "openvr_driver.h"
using namespace vr;

// Called by SteamVR at driver load; binds the driver context and registers the HMD device.
EVRInitError DeviceProvider::Init(IVRDriverContext* pDriverContext)
{
    EVRInitError initError = InitServerDriverContext(pDriverContext);
    if (initError != VRInitError_None)
    {
        return initError;
    }
    VRDriverLog()->Log("Initializing cardboardplusplus virtual HMD");
    m_hmdDriver = new HmdDriver();
    VRServerDriverHost()->TrackedDeviceAdded("cardboardplusplus_hmd", TrackedDeviceClass_HMD, m_hmdDriver);
    return VRInitError_None;
}

// Called by SteamVR at driver unload; destroys all devices created in Init.
void DeviceProvider::Cleanup()
{
    RemoveHandDevices();
    delete m_hmdDriver;
    m_hmdDriver = NULL;
}

// Returns the null-terminated interface version list SteamVR queries after HmdDriverFactory.
const char* const* DeviceProvider::GetInterfaceVersions()
{
    return k_InterfaceVersions;
}

// Called by SteamVR each frame; drives the HMD pose and spawns hand devices on
// first skeleton data.
void DeviceProvider::RunFrame()
{
    static int count = 0;
    count++;
    if (count == 1) VRDriverLog()->Log("DeviceProvider::RunFrame FIRST CALL");
    m_hmdDriver->RunFrame();
    EnsureHandDevices();
    for (int i = 0; i < 2; i++) {
        if (!m_handDevices[i] || !m_handRegistered[i]) continue;
        m_handDevices[i]->RunFrame();
    }
}

// Spawns hand devices when skeleton data first arrives and keeps them registered.
// OpenVR never removes a device mid-session, so the registration is permanent: once
// the hands stop appearing HandDriver reports deviceIsConnected=false instead, which
// is the signal games understand as a controller being put down.
void DeviceProvider::EnsureHandDevices()
{
    static const char* serials[2] = { "CBPP_HAND_LEFT", "CBPP_HAND_RIGHT" };
    for (int i = 0; i < 2; i++) {
        HmdDriver::SkeletonHand snap;
        if (!m_hmdDriver->GetSkeletonHand(i, snap)) continue;
        if (m_handRegistered[i]) continue;
        if (!m_handDevices[i]) m_handDevices[i] = new HandDriver(m_hmdDriver, i);
        if (!VRServerDriverHost()->TrackedDeviceAdded(
                serials[i], TrackedDeviceClass_Controller, m_handDevices[i])) {
            // Retrying every frame only spams vrserver with duplicate-serial adds;
            // one logged line is the report, and a SteamVR restart is the recovery.
            DriverLog("Hand device refused by SteamVR: %s", serials[i]);
        } else {
            DriverLog("Hand device spawned: %s", serials[i]);
        }
        m_handRegistered[i] = true;
    }
}

// Removal exists for driver shutdown; see EnsureHandDevices for why hands are not
// torn down during a session.
void DeviceProvider::RemoveHandDevices()
{
    for (int i = 0; i < 2; i++) {
        if (m_handDevices[i]) {
            delete m_handDevices[i];
            m_handDevices[i] = nullptr;
        }
        m_handRegistered[i] = false;
    }
}

// Tells SteamVR standby must stay disabled so sockets and encoder threads keep running.
bool DeviceProvider::ShouldBlockStandbyMode()
{
    return true;
}
void DeviceProvider::EnterStandby() {}
void DeviceProvider::LeaveStandby() {}