#include "DriverLog.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "openvr_driver.h"
// Formats a message and forwards it to SteamVR via VRDriverLog; called from every driver thread for diagnostics.
void DriverLog(const char* pFormat, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, pFormat);
    vsprintf_s(buffer, pFormat, args);
    strcat_s(buffer, "\n");
    vr::VRDriverLog()->Log(buffer);
    va_end(args);
}