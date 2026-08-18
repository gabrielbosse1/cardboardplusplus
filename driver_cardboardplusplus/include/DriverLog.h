#pragma once
// Driver-wide SteamVR log helper. Appends a newline and routes the message to
// the OpenVR driver log (avoiding the per-caller newline boilerplate).
//
// Lives in its own translation unit (DriverLog.cpp) so every subsystem can log
// without dragging in each other's implementation files.
void DriverLog(const char* pFormat, ...);