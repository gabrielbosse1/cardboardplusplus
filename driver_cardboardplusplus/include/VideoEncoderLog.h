// Encoder-scoped log helpers; ENCODER_LOG/ERROR tag output consumed in SteamVR logs during Present/encode.
#pragma once
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "openvr_driver.h"
inline void DriverLogFFmpeg(const char* pFormat, ...)
{
    char buffer[2048];
    va_list args;
    va_start(args, pFormat);
    vsprintf_s(buffer, pFormat, args);
    va_end(args);
    strcat_s(buffer, "\n");
    vr::VRDriverLog()->Log(buffer);
}
#define ENCODER_LOG(...) DriverLogFFmpeg("[VideoEncoder] " __VA_ARGS__)
#define ENCODER_ERROR(...) DriverLogFFmpeg("[VideoEncoder] ERROR: " __VA_ARGS__)