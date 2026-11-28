#include "HmdDriver.h"
#include "UdpSend.h"
#include "DriverLog.h"
#include "DebugLog.h"
#include "CardboardWire.h"
#include "H264Utils.h"
#include <cstdlib>
#include <cstring>
using namespace vr;
// The chunker/pacing/resync helpers live in include/UdpSend.h (shared with the
// transport unit tests, which exercise them against real loopback sockets).
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
    int bufSize = 4 * 1024 * 1024;
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
    bool allSent = true;
    if (m_previewEnabled.load(std::memory_order_relaxed)) {
        allSent = SendFramedUdp(m_udpSocket, &m_previewAddr, raw, rawSize, &m_udpDroppedPreview) && allSent;
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
        allSent = SendFramedUdp(m_udpSocket, &phoneTarget, framed, framedSize, &m_udpDroppedPhone) && allSent;
    } else {
        static uint64_t noTargetCounter = 0;
        if (++noTargetCounter % 300 == 1) {
            DriverLog("[UDP] No phone target set (skipped phone send, frames=%llu)", (unsigned long long)noTargetCounter);
        }
    }
    // IDR resync: a frame abandoned mid-send leaves its trailing slices lost;
    // Annex-B decoders (bridge preview ffmpeg, phone) conceal those rows with
    // smear artifacts until the next IDR. Two consecutive failed sends arm a
    // keyframe so the stream snaps back to a decodable state as soon as
    // capacity recovers; any fully delivered frame resets the streak.
    if (rawSize > 0) {
        if (udp::UdpArmResync(allSent, m_udpConsecutiveDrops,
                              m_udpDroppedPreview + m_udpDroppedPhone) &&
            m_pVideoEncoder) {
            m_pVideoEncoder->RequestKeyframe();
        }
    }
}