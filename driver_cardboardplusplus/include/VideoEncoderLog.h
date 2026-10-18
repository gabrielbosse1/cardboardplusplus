// Encoder-scoped log helpers; ENCODER_LOG/ERROR tag output consumed in SteamVR logs during Present/encode.
#pragma once
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "openvr_driver.h"
#include "Platform.h"
inline void DriverLogFFmpeg(const char* pFormat, ...)
{
    char buffer[2048];
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
#define ENCODER_LOG(...) DriverLogFFmpeg("[VideoEncoder] " __VA_ARGS__)
#define ENCODER_ERROR(...) DriverLogFFmpeg("[VideoEncoder] ERROR: " __VA_ARGS__)