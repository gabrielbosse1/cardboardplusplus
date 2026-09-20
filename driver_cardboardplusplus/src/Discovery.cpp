#include "HmdDriver.h"
#include "DriverLog.h"
#include "DebugLog.h"
#include "CardboardWire.h"
#include "DriverVersion.h"
#include <cstdio>
#include <cstring>
using namespace vr;
namespace {
constexpr char kPhoneDiscovery[] = "CARDBOARD_DISCOVERY";
constexpr char kPhoneHello[] = "CARDBOARD_PHONE_HELLO";
}
// Local allowlist strings for phone beacons; kept beside the wire header for the discovery matcher below.
// Accepts CARDBOARD_DISCOVERY and CARDBOARD_PHONE_HELLO; buf holds the datagram, len its size.
static bool IsPhoneDiscoveryPacket(const char* buf, int len)
{
    constexpr int kDiscoveryLen = (int)sizeof(kPhoneDiscovery) - 1;
    constexpr int kHelloLen = (int)sizeof(kPhoneHello) - 1;
    if (len >= kDiscoveryLen && strncmp(buf, kPhoneDiscovery, kDiscoveryLen) == 0)
        return true;
    if (len >= kHelloLen && strncmp(buf, kPhoneHello, kHelloLen) == 0)
        return true;
    return false;
}
// Binds UDP 42070 and starts the discovery thread; called from HmdDriver::Activate.
bool HmdDriver::InitializeDiscovery()
{
    DriverLog("Initializing UDP discovery socket...");
    m_discoverySocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_discoverySocket == INVALID_SOCKET) {
        DriverLog("discovery socket() failed! WSAError: %d", WSAGetLastError());
        return false;
    }
    BOOL broadcast = TRUE;
    setsockopt(m_discoverySocket, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcast, sizeof(broadcast));
    BOOL reuseAddr = TRUE;
    setsockopt(m_discoverySocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuseAddr, sizeof(reuseAddr));
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(wire::kDiscoveryPort);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(m_discoverySocket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        DriverLog("discovery bind() failed! WSAError: %d", WSAGetLastError());
        closesocket(m_discoverySocket);
        m_discoverySocket = INVALID_SOCKET;
        return false;
    }
    m_discoveryInitialized = true;
    m_discoveryRunning = true;
    DWORD recvTimeout = 1000;
    setsockopt(m_discoverySocket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recvTimeout, sizeof(recvTimeout));
    m_discoveryThread = std::thread(&HmdDriver::DiscoveryThreadFunc, this);
    DriverLog("UDP discovery socket initialized. Listening on port %d", wire::kDiscoveryPort);
    return true;
}
// Stops the discovery thread via a loopback wake packet, then closes the socket; called from Deactivate.
void HmdDriver::ShutdownDiscovery()
{
    DriverLog("Shutting down UDP discovery...");
    if (m_discoveryInitialized) {
        m_discoveryRunning = false;
        SOCKET wakeSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (wakeSocket != INVALID_SOCKET) {
            sockaddr_in localAddr;
            localAddr.sin_family = AF_INET;
            localAddr.sin_port = htons(wire::kDiscoveryPort);
            inet_pton(AF_INET, "127.0.0.1", &localAddr.sin_addr);
            sendto(wakeSocket, wire::kDiscoveryWakeup, (int)wire::kDiscoveryWakeupLen, 0,
                   (sockaddr*)&localAddr, sizeof(localAddr));
            closesocket(wakeSocket);
        }
        if (m_discoveryThread.joinable()) {
            m_discoveryThread.join();
        }
        if (m_discoverySocket != INVALID_SOCKET) {
            closesocket(m_discoverySocket);
            m_discoverySocket = INVALID_SOCKET;
        }
        m_discoveryInitialized = false;
    }
    DriverLog("UDP discovery shutdown complete.");
}
// Watches the 42070 inbox on the discovery thread; CAP clamps the encoder, BRIDGE_* lines get ACK/stats/toggles, phone beacons get an ACK.
void HmdDriver::DiscoveryThreadFunc()
{
    DriverLog("Discovery thread started");
    char buffer[256] = {};
    sockaddr_in senderAddr;
    int senderAddrLen = sizeof(senderAddr);
    static const DWORD kPhoneTimeoutMs = 5000;
    while (m_discoveryRunning) {
        int bytesReceived = recvfrom(m_discoverySocket, buffer, sizeof(buffer) - 1, 0,
                                     (sockaddr*)&senderAddr, &senderAddrLen);
        if (!m_discoveryRunning) {
            break;
        }
        if (bytesReceived == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) {
            } else {
                DriverLog("Discovery recvfrom error: %d", err);
            }
        }
        if (bytesReceived > 0) {
            buffer[bytesReceived] = '\0';
            char senderIpStr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &senderAddr.sin_addr, senderIpStr, sizeof(senderIpStr));
            DriverLog("Discovery packet received from %s:%d (size=%d, data='%s')",
                      senderIpStr, ntohs(senderAddr.sin_port), bytesReceived, buffer);
            if (strncmp(buffer, wire::kCardboardCap, wire::kCardboardCapLen) == 0) {
                int capW = 0;
                int capH = 0;
                if (sscanf_s(buffer, "CARDBOARD_CAP %d %d", &capW, &capH) == 2) {
                    DriverLog("Hardware decoder cap received from %s: %dx%d", senderIpStr, capW, capH);
                    ApplyHardwareCap(capW, capH);
                } else {
                    DriverLog("Malformed CARDBOARD_CAP from %s ignored: '%s'", senderIpStr, buffer);
                }
                continue;
            }
            if (strncmp(buffer, wire::kBridgeHeartbeat, wire::kBridgeHeartbeatLen) == 0) {
                char ack[64];
                int ackLen = snprintf(ack, sizeof(ack), "%s %s", wire::kBridgeAck, DRIVER_BUILD_VERSION);
                if (ackLen <= 0 || ackLen >= (int)sizeof(ack)) {
                    ackLen = (int)wire::kBridgeAckLen;
                    memcpy(ack, wire::kBridgeAck, ackLen);
                }
                sockaddr_in responseAddr;
                responseAddr.sin_family = AF_INET;
                responseAddr.sin_port = senderAddr.sin_port;
                responseAddr.sin_addr.s_addr = senderAddr.sin_addr.s_addr;
                sendto(m_discoverySocket, ack, ackLen, 0,
                       (sockaddr*)&responseAddr, sizeof(responseAddr));
                SendBridgeStats(responseAddr);
                DebugLog("BRIDGE_HELLO from %s:%d acked with BRIDGE_ACK + BRIDGE_STATS (data target untouched)", senderIpStr, ntohs(senderAddr.sin_port));
                continue;
            }
            if (strncmp(buffer, wire::kBridgePreview, wire::kBridgePreviewLen) == 0) {
                int previewVal = -1;
                if (sscanf_s(buffer, "BRIDGE_PREVIEW %d", &previewVal) == 1) {
                    bool enabled = (previewVal != 0);
                    m_previewEnabled.store(enabled, std::memory_order_relaxed);
                    DebugLog("BRIDGE_PREVIEW from %s:%d -> local preview %s", senderIpStr, ntohs(senderAddr.sin_port), enabled ? "ON" : "OFF");
                } else {
                    DebugLog("BRIDGE_PREVIEW from %s:%d malformed, ignored", senderIpStr, ntohs(senderAddr.sin_port));
                }
                continue;
            }
            if (strncmp(buffer, wire::kBridgeCfg, wire::kBridgeCfgLen) == 0) {
                int cfgFps = 0, cfgKbps = 0;
                char cfgCodec[32] = "";
                if (sscanf_s(buffer, "BRIDGE_CFG %d %d %31s", &cfgFps, &cfgKbps, cfgCodec, (unsigned)sizeof(cfgCodec)) >= 2) {
                    DriverLog("BRIDGE_CFG from %s:%d -> fps=%d bitrate=%d kbps codec=%s",
                              senderIpStr, ntohs(senderAddr.sin_port), cfgFps, cfgKbps, cfgCodec);
                    ApplyBridgeCfg(cfgFps, cfgKbps, cfgCodec);
                } else {
                    DebugLog("BRIDGE_CFG from %s:%d malformed, ignored", senderIpStr, ntohs(senderAddr.sin_port));
                }
                continue;
            }
            if (strncmp(buffer, wire::kKeyframeReq, wire::kKeyframeReqLen) == 0) {
                m_lastPhonePacketMs.store(GetTickCount64(), std::memory_order_relaxed);
                if (m_pVideoEncoder) {
                    m_pVideoEncoder->RequestKeyframe();
                }
                DebugLog("KEYFRAME_REQ from %s:%d -> forced IDR requested", senderIpStr, ntohs(senderAddr.sin_port));
                continue;
            }
            if (IsPhoneDiscoveryPacket(buffer, bytesReceived)) {
                SwitchDataTarget(senderIpStr);
                m_lastPhonePacketMs.store(GetTickCount64(), std::memory_order_relaxed);
                sockaddr_in responseAddr;
                responseAddr.sin_family = AF_INET;
                responseAddr.sin_port = senderAddr.sin_port;
                responseAddr.sin_addr.s_addr = senderAddr.sin_addr.s_addr;
                sendto(m_discoverySocket, wire::kDiscoveryAck, (int)wire::kDiscoveryAckLen, 0,
                       (sockaddr*)&responseAddr, sizeof(responseAddr));
                DebugLog("Discovery ACK sent to %s", senderIpStr);
            } else {
                static uint64_t unknownCount = 0;
                if (++unknownCount <= 3 || unknownCount % 50 == 1) {
                    DriverLog("Discovery: ignoring unknown packet from %s:%d (size=%d, count=%llu)",
                              senderIpStr, ntohs(senderAddr.sin_port), bytesReceived,
                              (unsigned long long)unknownCount);
                }
            }
        }
        if (m_hasPhoneTarget.load(std::memory_order_relaxed)) {
            long long now = GetTickCount64();
            long long last = m_lastPhonePacketMs.load(std::memory_order_relaxed);
            if (last > 0 && (now - last) > kPhoneTimeoutMs) {
                DriverLog("Phone timed out (%lld ms since last packet), clearing data target", now - last);
                m_hasPhoneTarget.store(false, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(m_targetIpMutex);
                m_serverAddr.sin_addr.s_addr = INADDR_ANY;
            }
        }
    }
    DriverLog("Discovery thread exiting");
}
// Replies to a BRIDGE_HELLO sender with fps/bitrate/frames/drops; addr is the hello source.
void HmdDriver::SendBridgeStats(const sockaddr_in& addr)
{
    char stats[160];
    int n = snprintf(stats, sizeof(stats), "%s fps=%d bitrate=%d frames=%llu drops=%u",
                     wire::kBridgeStats, m_encoderFps, m_encoderBitrate / 1000,
                     (unsigned long long)m_udpFramesSent.load(std::memory_order_relaxed),
                     m_udpDroppedPreview + m_udpDroppedPhone);
    if (n > 0 && n < (int)sizeof(stats)) {
        sendto(m_discoverySocket, stats, n, 0, (sockaddr*)&addr, sizeof(addr));
    }
}
// Points the phone UDP target at phoneIp:42069 (preview to localhost stays on); re-arms the 5 s phone timeout.
void HmdDriver::SwitchDataTarget(const char* phoneIp)
{
    std::lock_guard<std::mutex> lock(m_targetIpMutex);
    char currentIp[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &m_serverAddr.sin_addr, currentIp, sizeof(currentIp));
    if (strcmp(currentIp, phoneIp) == 0) {
        m_hasPhoneTarget.store(true, std::memory_order_relaxed);
        DebugLog("Data target still %s, re-armed phone send", phoneIp);
        return;
    }
    DriverLog("Switching data target from %s to %s", currentIp, phoneIp);
    m_serverAddr.sin_family = AF_INET;
    m_serverAddr.sin_port = htons(wire::kDataPort);
    if (inet_pton(AF_INET, phoneIp, &m_serverAddr.sin_addr) != 1) {
        DriverLog("SwitchDataTarget: inet_pton rejected '%s', target unchanged", phoneIp);
        return;
    }
    m_hasPhoneTarget.store(true, std::memory_order_relaxed);
    DebugLog("Data target switched to %s:%d (phone copy enabled alongside local preview)", phoneIp, wire::kDataPort);
}