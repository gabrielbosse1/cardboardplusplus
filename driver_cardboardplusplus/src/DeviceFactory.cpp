#include "DeviceProvider.h"
#include "openvr_driver.h"
#include <windows.h>
using namespace vr;
#define HMD_DLL_EXPORT extern "C" __declspec( dllexport )
static DeviceProvider g_deviceProvider;
// Single provider instance handed to SteamVR; lives for the lifetime of the driver DLL.
// SteamVR entry point: vrserver looks up IServerTrackedDeviceProvider and receives the instance above.
HMD_DLL_EXPORT
void* HmdDriverFactory(const char* interfaceName, int* returnCode)
{
	if (strcmp(interfaceName, IServerTrackedDeviceProvider_Version) == 0)
	{
		return &g_deviceProvider;
	}
	if (returnCode)
	{
		*returnCode = vr::VRInitError_Init_InterfaceNotFound;
	}
	return NULL;
}