#include "DriverLog.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "openvr_driver.h"
#include "Platform.h"

// Formats a message and forwards it to SteamVR via VRDriverLog; called from every driver thread for
// diagnostics. Falls back to stderr while the log host is unavailable (startup, early init failure).
void DriverLog(const char* pFormat, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, pFormat);
    vsnprintf(buffer, sizeof(buffer), pFormat, args);
    va_end(args);
    size_t n = strlen(buffer);
    if (n + 2 < sizeof(buffer)) {
        buffer[n] = '\n';
        buffer[n + 1] = '\0';
    }
    if (vr::VRDriverLog())
        vr::VRDriverLog()->Log(buffer);
    else
        cbpp::PlatformLogStderr(buffer);
}