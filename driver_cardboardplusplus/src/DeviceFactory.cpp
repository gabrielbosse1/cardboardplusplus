#include <DeviceProvider.h>
#include <openvr_driver.h>
#include <windows.h>

// DLL export macro for SteamVR driver interface.
#define HMD_DLL_EXPORT extern "C" __declspec( dllexport )

// Global provider instance for SteamVR to load.
DeviceProvider deviceProvider;

/**
This method returns an instance of your provider that OpenVR uses.
**/
HMD_DLL_EXPORT
void* HmdDriverFactory(const char* interfaceName, int* returnCode)
{
	if (strcmp(interfaceName, IServerTrackedDeviceProvider_Version) == 0) 
	{
		return &deviceProvider;
	}

	if (returnCode)
	{
		*returnCode = vr::VRInitError_Init_InterfaceNotFound;
	}

	return NULL;
}