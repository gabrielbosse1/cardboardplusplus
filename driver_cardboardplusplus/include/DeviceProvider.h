#pragma once
#include <ControllerDriver.h>
#include <HmdDriver.h>
#include <BridgeConnection.h>
#include <openvr_driver.h>
#include <windows.h>

using namespace vr;

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

    BridgeConnection* GetBridgeConnection() { return bridgeConnection; }

private:
    ControllerDriver* controllerDriver;
    HmdDriver* hmdDriver;
    BridgeConnection* bridgeConnection;
};
