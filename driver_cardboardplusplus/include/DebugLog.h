#pragma once
// Conditional debug logging for the SteamVR driver. Debug builds always log;
// release builds (NDEBUG) only log when CARDBOARD_DEBUG=1 is set in the
// environment, keeping the production SteamVR log file (vrserver.txt) free of
// DEBUG lines by default. Warnings and errors always fire.
//
// Usage:
//   DebugLog("sensor pkt #%d: gyro=(%.3f,%.3f,%.3f)", count, x, y, z);
//   DebugLogWarn("something unexpected: %s", msg);
//   DebugLogError("fatal: %s", msg);

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <windows.h>
#include "openvr_driver.h"

namespace cbpp_debug {

// Check once at load time; env var can't change mid-session (SteamVR restarts
// the driver DLL anyway). Debug builds always return true; release builds
// (NDEBUG) honor the env var so release defaults off with opt-in.
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

inline void log_msg(const char* tag, const char* pFormat, ...) {
    char buffer[2048];
    va_list args;
    va_start(args, pFormat);
    vsprintf_s(buffer, pFormat, args);
    va_end(args);
    // Prepend tag
    char tagged[2100];
    sprintf_s(tagged, "[%s] %s", tag, buffer);
    strcat_s(tagged, "\n");
    vr::VRDriverLog()->Log(tagged);
}

} // namespace cbpp_debug

// Debug log — always fires in debug builds; in release (NDEBUG) only when
// CARDBOARD_DEBUG=1, so release vrserver.txt stays clean by default.
#define DebugLog(...) \
    do { if (cbpp_debug::debug_enabled()) cbpp_debug::log_msg("DEBUG", __VA_ARGS__); } while(0)

// Warning — always fires (uses DriverLog under the hood).
#define DebugLogWarn(...) \
    do { cbpp_debug::log_msg("WARN", __VA_ARGS__); } while(0)

// Error — always fires.
#define DebugLogError(...) \
    do { cbpp_debug::log_msg("ERROR", __VA_ARGS__); } while(0)

// Throttled debug: fires every N calls. Use a static counter per callsite.
// Example: DebugLogThrottle(100, "frame %d", counter);
// The counter must be a local or static int/uint.
#define DebugLogThrottle(interval, ...) \
    do { \
        static unsigned long _dbg_cnt = 0; \
        _dbg_cnt++; \
        if (cbpp_debug::debug_enabled() && (_dbg_cnt % (interval) == 1)) \
            cbpp_debug::log_msg("DEBUG", __VA_ARGS__); \
    } while(0)
