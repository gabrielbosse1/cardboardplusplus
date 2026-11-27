#include "HmdDriver.h"
#include "DriverLog.h"
#include "DebugLog.h"
#include "CardboardWire.h"
#include "H264Utils.h"
#include "PreviewFraming.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
using namespace vr;
namespace {
static constexpr int kUdpChunkBytes = 1400;
// Session ids only need to differ between consecutive encoder lifetimes, including
// across a SteamVR restart (the counter alone would restart at the same value).
static uint32_t NextPreviewSessionId()
{
    static std::atomic<uint32_t> counter{0};
    uint64_t t = (uint64_t)std::chrono::system_clock::now().time_since_epoch().count();
    return (uint32_t)(t ^ (t >> 32)) + counter.fetch_add(1) * 0x9E3779B9u;
}
// Chunk size for non-blocking UDP sends; keeps each datagram inside a safe MTU.
// Fragments one buffer into 1400-byte datagrams on socket. A full send buffer
// abandons the whole buffer rather than its tail: the phone parses the stream as
// 4-byte length-prefixed frames, so a partial frame would leave its declared
// length unfulfilled and desync the reassembler for the rest of the stream.
static void SendFramedUdp(cbpp::PlatformSocket socket, const sockaddr_in* addr,
                          const uint8_t* framed, int framedSize,
                          uint32_t* droppedCounter)
{
    int offset = 0;
    while (offset < framedSize) {
        int chunkSize = (framedSize - offset > kUdpChunkBytes) ? kUdpChunkBytes : (framedSize - offset);
        long res = (long)sendto(socket, (const char*)(framed + offset), chunkSize, 0,
                                (sockaddr*)addr, sizeof(*addr));
        if (res < 0) {
            int err = cbpp::PlatformSocketError();
            if (cbpp::PlatformWouldBlock(err)) {
                (*droppedCounter)++;
                if (*droppedCounter % 30 == 1) {
                    DriverLog("[UDP] Send buffer full, dropping frame (dropped=%d)", (int)*droppedCounter);
                }
                return;
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
// Starts the socket layer plus the non-blocking video socket; preview points at localhost, the phone target waits for discovery.
bool HmdDriver::InitializeUDP()
{
    DriverLog("Initializing UDP socket...");
    if (!cbpp::PlatformStartupNetworking()) {
        DriverLog("Socket startup failed! Error: %d", cbpp::PlatformSocketError());
        return false;
    }
    m_udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_udpSocket == cbpp::kInvalidSocket) {
        DriverLog("socket() failed! Error: %d", cbpp::PlatformSocketError());
        cbpp::PlatformShutdownNetworking();
        return false;
    }
    int bufSize = 1024 * 1024;
    setsockopt(m_udpSocket, SOL_SOCKET, SO_SNDBUF, (const char*)&bufSize, sizeof(bufSize));
    cbpp::PlatformSetNonBlocking(m_udpSocket);
    m_previewAddr.sin_family = AF_INET;
    m_previewAddr.sin_port = htons(wire::kDataPort);
    inet_pton(AF_INET, "127.0.0.1", &m_previewAddr.sin_addr);
    m_serverAddr.sin_family = AF_INET;
    m_serverAddr.sin_port = htons(wire::kDataPort);
    m_serverAddr.sin_addr.s_addr = INADDR_ANY;
    m_hasPhoneTarget.store(false, std::memory_order_relaxed);
    m_udpInitialized = true;
    BeginPreviewSession("udp init");
    DriverLog("UDP socket initialized. Preview target 127.0.0.1:%d always on; phone target added on discovery", wire::kDataPort);
    return true;
}
// Closes the video socket and releases the socket layer; called from Deactivate.
void HmdDriver::ShutdownUDP()
{
    DriverLog("Shutting down UDP socket...");
    if (m_udpSocket != cbpp::kInvalidSocket) {
        cbpp::PlatformCloseSocket(m_udpSocket);
        m_udpSocket = cbpp::kInvalidSocket;
    }
    m_udpInitialized = false;
    cbpp::PlatformShutdownNetworking();
    DriverLog("UDP socket shutdown complete.");
}
// Encoder callback on the encoding thread; repairs a missing IDR marker, adds the 4-byte length header for the phone, and fans out.
void HmdDriver::OnEncodedPacket(uint8_t* data, int size, int64_t pts, bool keyframe)
{
    if (!m_udpInitialized || m_udpSocket == cbpp::kInvalidSocket || !data || size <= 0) {
        return;
    }
    static bool loggedFirstPacket = false;
    if (!loggedFirstPacket) {
        loggedFirstPacket = true;
        DriverLog("UDP: first encoded packet size=%d keyframe=%d preview=%d", size,
                  keyframe ? 1 : 0, m_previewEnabled.load(std::memory_order_relaxed) ? 1 : 0);
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
        SendFannedOut(fixed, fixedSize, framed, framedSize, keyframe);
        DebugLog("[UDP] Fixed keyframe: inserted IDR start code at offset %d", ppsEnd);
        return;
    }
    int framedSize = size + 4;
    uint8_t* framed = ensureScratch(m_scratchFramed, (size_t)framedSize);
    writeLengthPrefixed(data, size, framed);
    SendFannedOut(data, size, framed, framedSize, keyframe);
    DebugLog("[UDP] Sent framed packet: payload=%d bytes", size);
}
// Starts a new preview session: frame ids restart at 0 and the bridge re-acquires
// on the next keyframe. Called on UDP init and every encoder (re)init.
void HmdDriver::BeginPreviewSession(const char* reason)
{
    uint32_t session = NextPreviewSessionId();
    m_previewSession.store(session, std::memory_order_relaxed);
    DriverLog("[Preview] new session %08X (%s)", session, reason);
}
// Tells the bridge preview the current session is over (STREAM_END control
// datagram, see CardboardWire.h) so it clears to idle immediately. Best effort:
// a lost goodbye falls back to the bridge's receive timeout. Safe from any
// thread; it touches only the atomic session and the socket.
void HmdDriver::SendPreviewEnd(const char* reason)
{
    if (!m_udpInitialized || m_udpSocket == cbpp::kInvalidSocket) return;
    uint32_t session = m_previewSession.load(std::memory_order_relaxed);
    uint8_t datagram[wire::kPreviewHeaderSize];
    std::size_t len = preview::WriteEnd(datagram, session);
    // Two copies: localhost loss is rare but the goodbye is a single datagram.
    for (int i = 0; i < 2; i++) {
        sendto(m_udpSocket, (const char*)datagram, (int)len, 0,
               (sockaddr*)&m_previewAddr, sizeof(m_previewAddr));
    }
    DriverLog("[Preview] STREAM_END session %08X (%s)", session, reason);
}
// Sends one access unit to the bridge preview as header-framed fragments (see
// CardboardWire.h). Runs on the encoding thread only. A send failure stops the
// frame; the bridge sees the missing fragments and drops the frame instead of
// decoding a truncated one.
void HmdDriver::SendPreviewFrame(const uint8_t* raw, int rawSize, bool keyframe)
{
    uint32_t session = m_previewSession.load(std::memory_order_relaxed);
    if (session != m_previewSentSession) {
        m_previewSentSession = session;
        m_previewFrameId = 0;
    }
    uint32_t frameId = m_previewFrameId++;
    uint8_t scratch[wire::kPreviewMaxDatagram];
    int sendErr = 0;
    auto sink = [&](const uint8_t* datagram, std::size_t len) -> bool {
        long res = (long)sendto(m_udpSocket, (const char*)datagram, (int)len, 0,
                                (sockaddr*)&m_previewAddr, sizeof(m_previewAddr));
        if (res < 0) {
            sendErr = cbpp::PlatformSocketError();
            return false;
        }
        return true;
    };
    preview::SendResult r = preview::SendFrame(raw, (uint32_t)rawSize, session, frameId,
                                               keyframe, scratch, sink);
    if (!r.complete()) {
        m_udpDroppedPreview++;
        if (m_udpDroppedPreview % 30 == 1) {
            DriverLog("[Preview] frame %u incomplete: %u/%u fragments sent (err=%d%s, dropped=%u)",
                      frameId, r.sent, r.fragments, sendErr,
                      cbpp::PlatformWouldBlock(sendErr) ? " would-block" : "",
                      m_udpDroppedPreview);
        }
    }
    DebugLog("[Preview] session=%08X frame=%u size=%d key=%d frags=%u sent=%u",
             session, frameId, rawSize, keyframe ? 1 : 0, r.fragments, r.sent);
}
// Sends the framed preview copy to localhost and the length-headed copy to the phone when discovery set a target.
void HmdDriver::SendFannedOut(const uint8_t* raw, int rawSize,
                               const uint8_t* framed, int framedSize, bool keyframe)
{
    if (m_previewEnabled.load(std::memory_order_relaxed)) {
        SendPreviewFrame(raw, rawSize, keyframe);
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