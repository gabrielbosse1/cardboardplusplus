#pragma once
// Thin OS shim: sockets, timing, environment and dynamic loading for the
// driver core. Windows keeps its native calls; Linux uses the POSIX
// equivalents. Types are spelled once here so the wire, transport and logging
// layers stay free of #ifdef.
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unistd.h>
#endif

namespace cbpp {

#ifdef _WIN32
using PlatformSocket = SOCKET;
static const PlatformSocket kInvalidSocket = INVALID_SOCKET;

using PlatformModule = HMODULE;

// Winsock needs an explicit startup/teardown; POSIX sockets do not.
inline bool PlatformStartupNetworking() {
    WSADATA wsaData;
    return WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
}
inline void PlatformShutdownNetworking() { WSACleanup(); }
inline void PlatformCloseSocket(PlatformSocket s) { closesocket(s); }
inline int PlatformSocketError() { return WSAGetLastError(); }
inline bool PlatformWouldBlock(int err) { return err == WSAEWOULDBLOCK; }
inline bool PlatformTimedOut(int err) { return err == WSAETIMEDOUT; }
inline bool PlatformInterrupted(int err) { return err == WSAEINTR; }
inline void PlatformSetNonBlocking(PlatformSocket s) {
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
}
inline PlatformModule PlatformLoadModule(const char* name) { return LoadLibraryA(name); }
inline void PlatformFreeModule(PlatformModule m) { FreeLibrary(m); }
inline void* PlatformGetSymbol(PlatformModule m, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(m, name));
}
#else
using PlatformSocket = int;
static const PlatformSocket kInvalidSocket = -1;

using PlatformModule = void*;

inline bool PlatformStartupNetworking() { return true; }
inline void PlatformShutdownNetworking() {}
inline void PlatformCloseSocket(PlatformSocket s) { ::close(s); }
inline int PlatformSocketError() { return errno; }
inline bool PlatformWouldBlock(int err) { return err == EAGAIN || err == EWOULDBLOCK; }
inline bool PlatformTimedOut(int err) { return err == EAGAIN || err == EWOULDBLOCK; }
inline bool PlatformInterrupted(int err) { return err == EINTR; }
inline void PlatformSetNonBlocking(PlatformSocket s) {
    int flags = fcntl(s, F_GETFL, 0);
    if (flags >= 0)
        fcntl(s, F_SETFL, flags | O_NONBLOCK);
}
inline PlatformModule PlatformLoadModule(const char* name) { return dlopen(name, RTLD_NOW | RTLD_LOCAL); }
inline void PlatformFreeModule(PlatformModule m) { dlclose(m); }
inline void* PlatformGetSymbol(PlatformModule m, const char* name) { return dlsym(m, name); }
#endif

// Receive timeout for the discovery and sensor loops. Windows takes milliseconds,
// POSIX takes a timeval, so the two shapes live here rather than at every call site.
inline void PlatformSetRecvTimeout(PlatformSocket s, uint32_t ms) {
#ifdef _WIN32
    DWORD tv = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = (long)(ms / 1000);
    tv.tv_usec = (long)((ms % 1000) * 1000);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

// Milliseconds from a monotonic clock; only differences are meaningful.
inline uint64_t PlatformNowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Microseconds from a monotonic clock; only differences are meaningful.
inline uint64_t PlatformNowUs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline void PlatformSleepMs(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Environment lookup; returns nullptr when unset, the caller must not free.
inline const char* PlatformGetEnv(const char* name) {
#ifdef _WIN32
    static thread_local char value[256];
    DWORD n = GetEnvironmentVariableA(name, value, sizeof(value));
    return (n > 0 && n < sizeof(value)) ? value : nullptr;
#else
    return getenv(name);
#endif
}

// True when CARDBOARD_DEBUG=1.
inline bool PlatformDebugEnv() {
    const char* v = PlatformGetEnv("CARDBOARD_DEBUG");
    return v && v[0] == '1';
}

// stderr sink used before the SteamVR log host exists.
inline void PlatformLogStderr(const char* text) { fputs(text, stderr); fflush(stderr); }

// Parses "<prefix> <int> [int]" into a and b; returns false on a prefix mismatch
// or a missing/garbage number, so callers can log the exact datagram instead.
inline bool PlatformParseInts(const char* text, const char* prefix, int& a, int& b) {
    size_t n = strlen(prefix);
    if (strncmp(text, prefix, n) != 0)
        return false;
    const char* p = text + n;
    char* end = nullptr;
    long first = strtol(p, &end, 10);
    if (end == p)
        return false;
    p = end;
    while (*p == ' ')
        p++;
    if (*p == '\0') {
        a = (int)first;
        b = 0;
        return true;
    }
    long second = strtol(p, &end, 10);
    if (end == p)
        return false;
    a = (int)first;
    b = (int)second;
    return true;
}

// Parses "<prefix> <word>" into out (space delimited, max len-1 chars).
inline bool PlatformParseWord(const char* text, const char* prefix, char* out, size_t outSize) {
    size_t n = strlen(prefix);
    if (strncmp(text, prefix, n) != 0)
        return false;
    const char* p = text + n;
    while (*p == ' ')
        p++;
    size_t i = 0;
    while (p[i] && p[i] != ' ' && p[i] != '\r' && p[i] != '\n' && i + 1 < outSize) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    return i > 0;
}

// Shared memory for the bridge endpoint. Windows keeps a file-mapping handle, POSIX a shm fd;
// both are opaque here so BridgeServer stays free of #ifdef. Region names are the same on
// both platforms except for the leading slash POSIX shm_open requires.
#ifdef _WIN32
using PlatformMapping = void*;
static const PlatformMapping kInvalidMapping = INVALID_HANDLE_VALUE;
inline std::wstring PlatformWideName(const char* name) {
    int n = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
    if (n <= 0)
        return std::wstring();
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name, -1, &out[0], n);
    out.resize((size_t)n - 1);
    return out;
}
inline PlatformMapping PlatformCreateMapping(const char* name, size_t size) {
    return CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                              (DWORD)(size >> 32), (DWORD)(size & 0xFFFFFFFFu),
                              PlatformWideName(name).c_str());
}
inline PlatformMapping PlatformOpenMapping(const char* name, size_t size) {
    (void)size;
    return OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, PlatformWideName(name).c_str());
}
inline uint8_t* PlatformMapView(PlatformMapping mapping, size_t size) {
    (void)size;
    return static_cast<uint8_t*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
}
inline void PlatformCloseMapping(PlatformMapping mapping) { CloseHandle(mapping); }
inline void PlatformUnmapView(uint8_t* base, size_t size) {
    (void)size;
    UnmapViewOfFile(base);
}
#else
using PlatformMapping = int;
static const PlatformMapping kInvalidMapping = -1;
inline PlatformMapping PlatformCreateMapping(const char* name, size_t size) {
    int fd = shm_open(name, O_CREAT | O_RDWR, 0600);
    if (fd < 0)
        return kInvalidMapping;
    if (ftruncate(fd, (off_t)size) != 0) {
        ::close(fd);
        shm_unlink(name);
        return kInvalidMapping;
    }
    return fd;
}
inline PlatformMapping PlatformOpenMapping(const char* name, size_t size) {
    int fd = shm_open(name, O_RDWR, 0600);
    if (fd < 0)
        return kInvalidMapping;
    struct stat st {};
    // Mapping past the end of the region raises SIGBUS on first touch, so a short
    // region from the other side is rejected here instead.
    if (fstat(fd, &st) != 0 || (size_t)st.st_size < size) {
        ::close(fd);
        return kInvalidMapping;
    }
    return fd;
}
inline uint8_t* PlatformMapView(PlatformMapping mapping, size_t size) {
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, mapping, 0);
    return p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
}
inline void PlatformCloseMapping(PlatformMapping mapping) {
    if (mapping >= 0)
        ::close(mapping);
}
inline void PlatformUnmapView(uint8_t* base, size_t size) {
    if (base)
        munmap(base, size);
}
#endif

} // namespace cbpp