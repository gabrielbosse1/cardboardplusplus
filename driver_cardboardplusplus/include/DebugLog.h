#pragma once
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <windows.h>
#include "openvr_driver.h"
// Gated verbose logging; enabled in debug builds and in release only via CARDBOARD_DEBUG=1.
namespace cbpp_debug {
// Reads CARDBOARD_DEBUG once per process in release; always true in debug builds.
inline bool debug_enabled() {
#ifdef NDEBUG
    static const bool enabled = [] {
        char val[2] = {};
        return GetEnvironmentVariableA("CARDBOARD_DEBUG", val, sizeof(val)) == 1 &&
               val[0] == '1';
    }();
    return enabled;
#else
    return true;
#endif
}
// Formats with a tag header and writes to VRDriverLog; backing routine for the DebugLog macros below.
inline void log_msg(const char* tag, const char* pFormat, ...) {
    char buffer[2048];
    va_list args;
    va_start(args, pFormat);
    vsprintf_s(buffer, pFormat, args);
    va_end(args);
    char tagged[2100];
    sprintf_s(tagged, "[%s] %s", tag, buffer);
    strcat_s(tagged, "\n");
    vr::VRDriverLog()->Log(tagged);
}
}
// Public macros: DebugLog/Throttle respect the gate, Warn/Error always emit; Throttle logs 1 in N calls per call-site.
#define DebugLog(...) \
    do { if (cbpp_debug::debug_enabled()) cbpp_debug::log_msg("DEBUG", __VA_ARGS__); } while(0)
#define DebugLogWarn(...) \
    do { cbpp_debug::log_msg("WARN", __VA_ARGS__); } while(0)
#define DebugLogError(...) \
    do { cbpp_debug::log_msg("ERROR", __VA_ARGS__); } while(0)
#define DebugLogThrottle(interval, ...) \
    do { \
        static unsigned long _dbg_cnt = 0; \
        _dbg_cnt++; \
        if (cbpp_debug::debug_enabled() && (_dbg_cnt % (interval) == 1)) \
            cbpp_debug::log_msg("DEBUG", __VA_ARGS__); \
    } while(0)
