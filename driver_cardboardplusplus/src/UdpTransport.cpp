#include "HmdDriver.h"
#include "DriverLog.h"
#include "DebugLog.h"
#include "CardboardWire.h"
#include "H264Utils.h"
#include <cstdlib>
#include <cstring>
using namespace vr;
namespace {
static constexpr int kUdpChunkBytes = 1400;
// Chunk size for non-blocking UDP sends; keeps each datagram inside a safe MTU.
// Fragments one buffer into 1400-byte datagrams on socket; counts a drop when the send buffer is full.
static void SendFramedUdp(SOCKET socket, const sockaddr_in* addr,
                          const uint8_t* framed, int framedSize,
                          uint32_t* droppedCounter)
{
    int offset = 0;
    while (offset < framedSize) {
        int chunkSize = (framedSize - offset > kUdpChunkBytes) ? kUdpChunkBytes : (framedSize - offset);
        int res = sendto(socket, (const char*)(framed + offset), chunkSize, 0,
                         (sockaddr*)addr, sizeof(*addr));
        if (res == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                (*droppedCounter)++;
                if (*droppedCounter % 30 == 1) {
                    DriverLog("[UDP] Send buffer full, dropping frame (dropped=%d)", (int)*droppedCounter);
                }
            } else {
                char targetIp[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &addr->sin_addr, targetIp, sizeof(targetIp));
                DriverLog("[UDP] sendto error %d to %s:%d (size=%d)", err,
                          targetIp, ntohs(addr->sin_port), chunkSize);
            }
            break;
        }
        offset += chunkSize;
    }
}
}
// Starts Winsock plus the non-blocking video socket; preview points at localhost, the phone target waits for discovery.
bool HmdDriver::InitializeUDP()
{
    DriverLog("Initializing UDP socket...");
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        DriverLog("WSAStartup failed! Error: %d", result);
        return false;
    }
    m_udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_udpSocket == INVALID_SOCKET) {
        DriverLog("socket() failed! WSAError: %d", WSAGetLastError());
        WSACleanup();
        return false;
    }
    int bufSize = 1024 * 1024;
    setsockopt(m_udpSocket, SOL_SOCKET, SO_SNDBUF, (const char*)&bufSize, sizeof(bufSize));
    u_long nonBlocking = 1;
    ioctlsocket(m_udpSocket, FIONBIO, &nonBlocking);
    m_previewAddr.sin_family = AF_INET;
    m_previewAddr.sin_port = htons(wire::kDataPort);
    inet_pton(AF_INET, "127.0.0.1", &m_previewAddr.sin_addr);
    m_serverAddr.sin_family = AF_INET;
    m_serverAddr.sin_port = htons(wire::kDataPort);
    m_serverAddr.sin_addr.s_addr = INADDR_ANY;
    m_hasPhoneTarget.store(false, std::memory_order_relaxed);
    m_udpInitialized = true;
    DriverLog("UDP socket initialized. Preview target 127.0.0.1:%d always on; phone target added on discovery", wire::kDataPort);
    return true;
}
// Closes the video socket and tears down Winsock; called from Deactivate.
void HmdDriver::ShutdownUDP()
{
    DriverLog("Shutting down UDP socket...");
    if (m_udpSocket != INVALID_SOCKET) {
        closesocket(m_udpSocket);
        m_udpSocket = INVALID_SOCKET;
    }
    m_udpInitialized = false;
    WSACleanup();
    DriverLog("UDP socket shutdown complete.");
}
// Encoder callback on the encoding thread; repairs a missing IDR marker, adds the 4-byte length header for the phone, and fans out.
void HmdDriver::OnEncodedPacket(uint8_t* data, int size, int64_t pts, bool keyframe)
{
    if (!m_udpInitialized || m_udpSocket == INVALID_SOCKET || !data || size <= 0) {
        return;
    }
    if (keyframe && size >= 8) {
        DebugLog("[Encoded] size=%d, pts=%lld, keyframe=YES, first16=%02X %02X %02X %02X %02X %02X %02X %02X",
                  size, pts,
                  data[0], data[1], data[2], data[3],
                  data[4], data[5], data[6], data[7]);
    } else {
        DebugLog("[Encoded] size=%d, pts=%lld, keyframe=%s, data[0]=0x%02X",
                  size, pts, keyframe ? "YES" : "NO", data[0]);
    }
    m_udpFramesSent.fetch_add(1, std::memory_order_relaxed);
    auto ensureScratch = [](std::vector<uint8_t>& buf, size_t need) -> uint8_t* {
        if (buf.size() < need) buf.resize(need);
        return buf.data();
    };
    auto writeLengthPrefixed = [](const uint8_t* src, int srcSize, uint8_t* dst) {
        dst[0] = (uint8_t)((srcSize >> 24) & 0xFF);
        dst[1] = (uint8_t)((srcSize >> 16) & 0xFF);
        dst[2] = (uint8_t)((srcSize >> 8) & 0xFF);
        dst[3] = (uint8_t)(srcSize & 0xFF);
        memcpy(dst + 4, src, srcSize);
    };
    static const uint8_t kIdrPrefix[] = { 0x00, 0x00, 0x00, 0x01, 0x65 };
    int ppsEnd = h264::FindIdrInsertionPoint(data, size, keyframe);
    if (ppsEnd > 0) {
        int fixedSize = size + 5;
        uint8_t* fixed = ensureScratch(m_scratchFixed, (size_t)fixedSize);
        memcpy(fixed, data, ppsEnd);
        memcpy(fixed + ppsEnd, kIdrPrefix, 5);
        memcpy(fixed + ppsEnd + 5, data + ppsEnd, size - ppsEnd);
        int framedSize = fixedSize + 4;
        uint8_t* framed = ensureScratch(m_scratchFramed, (size_t)framedSize);
        writeLengthPrefixed(fixed, fixedSize, framed);
        SendFannedOut(fixed, fixedSize, framed, framedSize);
        DebugLog("[UDP] Fixed keyframe: inserted IDR start code at offset %d", ppsEnd);
        return;
    }
    int framedSize = size + 4;
    uint8_t* framed = ensureScratch(m_scratchFramed, (size_t)framedSize);
    writeLengthPrefixed(data, size, framed);
    SendFannedOut(data, size, framed, framedSize);
    DebugLog("[UDP] Sent framed packet: payload=%d bytes", size);
}
// Sends raw Annex-B to the localhost preview and the length-headed copy to the phone when discovery set a target.
void HmdDriver::SendFannedOut(const uint8_t* raw, int rawSize,
                               const uint8_t* framed, int framedSize)
{
    if (m_previewEnabled.load(std::memory_order_relaxed)) {
        SendFramedUdp(m_udpSocket, &m_previewAddr, raw, rawSize, &m_udpDroppedPreview);
    }
    sockaddr_in phoneTarget{};
    bool hasTarget = false;
    {
        std::lock_guard<std::mutex> lock(m_targetIpMutex);
        hasTarget = m_hasPhoneTarget.load(std::memory_order_relaxed);
        if (hasTarget) phoneTarget = m_serverAddr;
    }
    if (hasTarget) {
        static uint64_t phoneLogCounter = 0;
        if (++phoneLogCounter % 60 == 1) {
            char phoneIp[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &phoneTarget.sin_addr, phoneIp, sizeof(phoneIp));
            DriverLog("[UDP] Sending to phone %s:%d (frames=%llu, raw=%d, framed=%d)",
                      phoneIp, ntohs(phoneTarget.sin_port),
                      (unsigned long long)phoneLogCounter, rawSize, framedSize);
        }
        SendFramedUdp(m_udpSocket, &phoneTarget, framed, framedSize, &m_udpDroppedPhone);
    } else {
        static uint64_t noTargetCounter = 0;
        if (++noTargetCounter % 300 == 1) {
            DriverLog("[UDP] No phone target set (skipped phone send, frames=%llu)", (unsigned long long)noTargetCounter);
        }
    }
}