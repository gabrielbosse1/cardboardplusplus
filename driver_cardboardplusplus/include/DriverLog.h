#pragma once
// printf-style forwarder to SteamVR's VRDriverLog; shared by all driver translation units.
void DriverLog(const char* pFormat, ...);