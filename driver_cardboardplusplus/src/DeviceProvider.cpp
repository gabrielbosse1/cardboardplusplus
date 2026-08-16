#include <DeviceProvider.h>

EVRInitError DeviceProvider::Init(IVRDriverContext* pDriverContext)
{
    EVRInitError initError = InitServerDriverContext(pDriverContext);
    if (initError != EVRInitError::VRInitError_None)
    {
        return initError;
    }
    
    VRDriverLog()->Log("Initializing Cardboard++ driver");

    bridgeConnection = new BridgeConnection();
    if (!bridgeConnection->Initialize()) {
        VRDriverLog()->Log("WARNING: Bridge connection initialization failed. Streaming will be disabled.");
    }

    controllerDriver = new ControllerDriver();
    VRServerDriverHost()->TrackedDeviceAdded("example_controller", TrackedDeviceClass_Controller, controllerDriver);

    VRDriverLog()->Log("Initializing Cardboard++ virtual HMD");
    hmdDriver = new HmdDriver();
    hmdDriver->SetBridgeConnection(bridgeConnection);
    VRServerDriverHost()->TrackedDeviceAdded("example_virtual_hmd", TrackedDeviceClass_HMD, hmdDriver);

    return vr::VRInitError_None;
}

void DeviceProvider::Cleanup()
{
    delete controllerDriver;
    controllerDriver = NULL;
    delete hmdDriver;
    hmdDriver = NULL;
    delete bridgeConnection;
    bridgeConnection = NULL;
}

const char* const* DeviceProvider::GetInterfaceVersions()
{
    return k_InterfaceVersions;
}

void DeviceProvider::RunFrame()
{
    controllerDriver->RunFrame();
    hmdDriver->RunFrame();
}

bool DeviceProvider::ShouldBlockStandbyMode()
{
    return false;
}

void DeviceProvider::EnterStandby() {}

void DeviceProvider::LeaveStandby() {}
