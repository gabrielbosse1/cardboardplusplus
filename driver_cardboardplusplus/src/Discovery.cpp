#include "HmdDriver.h"
#include "DriverLog.h"
#include "DebugLog.h"
#include "CardboardWire.h"
#include "DriverVersion.h"
#include <cstdio>
#include <cstring>

using namespace vr;

// ---------------------------------------------------------------------------
// Phone discovery: broadcast UDP exchange that learns the phone's IP and
// reports its hardware decoder cap.
//
// Only allowlisted phone packets switch the video target: "CARDBOARD_DISCOVERY"
// and "CARDBOARD_PHONE_HELLO*" (R2/M10 — legacy-tolerant, but scanners and
// random LAN noise must never steal the stream or earn an ACK). Everything
// else known (cap / bridge / keyframe) is handled in place; unknown packets
// are dropped with a rate-limited log.
// A "CARDBOARD_CAP <w> <h>" packet only reconfigures the encoder via
// ApplyHardwareCap() and is NOT acknowledged (see the wire contract in
// CardboardWire.h).
// ---------------------------------------------------------------------------

namespace {
// Phone identity tokens: matched by prefix so version-suffixed variants
// ("CARDBOARD_PHONE_HELLO v2") keep working. Kept here (not CardboardWire.h)
// because PHONE_HELLO is not part of the locked wire contract.
constexpr char kPhoneDiscovery[] = "CARDBOARD_DISCOVERY";
constexpr char kPhoneHello[] = "CARDBOARD_PHONE_HELLO";
} // namespace

// True when buf (len bytes) is an allowlisted phone packet. len-guarded:
// strncmp past the received bytes would read stale buffer contents (M10).
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

bool HmdDriver::InitializeDiscovery()
{
    DriverLog("Initializing UDP discovery socket...");

    m_discoverySocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_discoverySocket == INVALID_SOCKET) {
        DriverLog("discovery socket() failed! WSAError: %d", WSAGetLastError());
        return false;
    }

    // Allow broadcast reception
    BOOL broadcast = TRUE;
    setsockopt(m_discoverySocket, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcast, sizeof(broadcast));

    // Allow address reuse
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

    // Set a read timeout so the loop can check the phone timeout even when
    // no packets arrive (recvfrom returns WSAETIMEDOUT periodically).
    DWORD recvTimeout = 1000; // 1 second
    setsockopt(m_discoverySocket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recvTimeout, sizeof(recvTimeout));

    m_discoveryThread = std::thread(&HmdDriver::DiscoveryThreadFunc, this);

    DriverLog("UDP discovery socket initialized. Listening on port %d", wire::kDiscoveryPort);
    return true;
}

void HmdDriver::ShutdownDiscovery()
{
    DriverLog("Shutting down UDP discovery...");

    if (m_discoveryInitialized) {
        m_discoveryRunning = false;

        // Send a dummy packet to unblock recvfrom
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

void HmdDriver::DiscoveryThreadFunc()
{
    DriverLog("Discovery thread started");

    char buffer[256] = {}; // zeroed: prefix matches below must never read stale bytes past bytesReceived
    sockaddr_in senderAddr;
    int senderAddrLen = sizeof(senderAddr);

    // Phone timeout: if no phone packet arrives for this long, clear the
    // phone target so video stops being sent to a stale IP.
    static const DWORD kPhoneTimeoutMs = 5000;

    while (m_discoveryRunning) {
        int bytesReceived = recvfrom(m_discoverySocket, buffer, sizeof(buffer) - 1, 0,
                                     (sockaddr*)&senderAddr, &senderAddrLen);

        if (!m_discoveryRunning) {
            break;
        }

        // recvfrom returns -1 with WSAETIMEDOUT when the read timeout fires.
        // This is expected — we use the timeout to check the phone liveness.
        if (bytesReceived == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) {
                // Fall through to the phone timeout check below.
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
                // Phone is reporting its hardware decoder cap; no ACK needed.
                // Payload format matches the wire contract string exactly:
                //   "CARDBOARD_CAP <width> <height>"
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
                // The bridge (cardboard-bridge.exe) pings this socket to prove the
                // driver is alive. Reply with BRIDGE_ACK so the bridge marks us
                // driver_connected, then piggyback BRIDGE_STATS (the bridge's
                // heartbeat cadence drives the stats rate). NEVER touch the video
                // data target — the bridge lives on 127.0.0.1 and would otherwise
                // hijack the stream.
                // The ACK carries our build version ("BRIDGE_ACK v1 <count>")
                // so the bridge can show which commit this driver was built
                // from; old bridges only check the BRIDGE_ACK prefix.
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
                // Bridge toggles the localhost preview stream: "BRIDGE_PREVIEW 1"
                // keeps 127.0.0.1:42069 flowing (bridge UI / ffplay), "BRIDGE_PREVIEW 0"
                // cuts it. Only the preview target is affected, never the phone's.
                // Parsed as an explicit trailing token (L10): a strstr "1" check
                // would misfire on values like "10".
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
                // Bridge stream-settings push: "BRIDGE_CFG <fps> <bitrate_kbps> <codec>".
                // Applied live (same re-init path as the hardware-cap clamp);
                // malformed values keep the current encoder settings.
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
                // The phone lost video data (its reassembly desynced) and needs
                // a fresh reference frame. Force the next encoded frame to IDR.
                // NO ack and NO target switch: a reply would land on the
                // phone's video port and corrupt its reassembly buffer, and the
                // sender is already the phone. The NACK still proves the phone
                // is alive, so refresh its liveness timestamp.
                m_lastPhonePacketMs.store(GetTickCount64(), std::memory_order_relaxed);
                if (m_pVideoEncoder) {
                    m_pVideoEncoder->RequestKeyframe();
                }
                DebugLog("KEYFRAME_REQ from %s:%d -> forced IDR requested", senderIpStr, ntohs(senderAddr.sin_port));
                continue;
            }

            // Allowlisted phone packets only: switch the video target to the
            // sender's IP and ACK it (M10/R2). Anything else is LAN noise or a
            // scanner — dropped with a rate-limited log, no target switch, no ACK.
            if (IsPhoneDiscoveryPacket(buffer, bytesReceived)) {
                SwitchDataTarget(senderIpStr);
                m_lastPhonePacketMs.store(GetTickCount64(), std::memory_order_relaxed);

                // Send acknowledgment back to the phone (wire constant "ACK")
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

        // Check if the phone has timed out (no packets for kPhoneTimeoutMs).
        // This runs every recv iteration, which is fine since recvfrom blocks
        // until a packet arrives or the thread is shut down.
        if (m_hasPhoneTarget.load(std::memory_order_relaxed)) {
            long long now = GetTickCount64();
            long long last = m_lastPhonePacketMs.load(std::memory_order_relaxed);
            if (last > 0 && (now - last) > kPhoneTimeoutMs) {
                DriverLog("Phone timed out (%lld ms since last packet), clearing data target", now - last);
                m_hasPhoneTarget.store(false, std::memory_order_relaxed);
                // Clear the address too (under the same mutex SwitchDataTarget
                // uses) so a reconnect can't hit the "already set" early
                // return with a stale address and a cleared flag.
                std::lock_guard<std::mutex> lock(m_targetIpMutex);
                m_serverAddr.sin_addr.s_addr = INADDR_ANY;
            }
        }
    }

    DriverLog("Discovery thread exiting");
}

// Build + send the periodic streaming-stats packet to the bridge. Tied to the
// BRIDGE_HELLO cadence (sent right after every BRIDGE_ACK), so no extra timer
// thread is needed. Numbers are read without locks: fps/bitrate change only
// via settings, and the frame counters are monotonic atomics — a torn snapshot
// between them is fine for a monitoring packet.

void HmdDriver::SendBridgeStats(const sockaddr_in& addr)
{
    char stats[160];
    // "BRIDGE_STATS fps=<fps> bitrate=<kbps> frames=<n> drops=<n>"
    // Single snprintf (L7): no hand-rolled number appender, no stray-NUL risk.
    int n = snprintf(stats, sizeof(stats), "%s fps=%d bitrate=%d frames=%llu drops=%u",
                     wire::kBridgeStats, m_encoderFps, m_encoderBitrate / 1000,
                     (unsigned long long)m_udpFramesSent.load(std::memory_order_relaxed),
                     m_udpDroppedPreview + m_udpDroppedPhone);
    if (n > 0 && n < (int)sizeof(stats)) {
        sendto(m_discoverySocket, stats, n, 0, (sockaddr*)&addr, sizeof(addr));
    }
}

void HmdDriver::SwitchDataTarget(const char* phoneIp)
{
    std::lock_guard<std::mutex> lock(m_targetIpMutex);

    char currentIp[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &m_serverAddr.sin_addr, currentIp, sizeof(currentIp));

    if (strcmp(currentIp, phoneIp) == 0) {
        // Re-arm the flag: the phone-timeout path clears it without touching
        // the address, so a reconnecting phone would otherwise hit this
        // early return forever and never get video again.
        m_hasPhoneTarget.store(true, std::memory_order_relaxed);
        DebugLog("Data target still %s, re-armed phone send", phoneIp);
        return;
    }

    DriverLog("Switching data target from %s to %s", currentIp, phoneIp);

    m_serverAddr.sin_family = AF_INET;
    m_serverAddr.sin_port = htons(wire::kDataPort);
    // L11: never install a garbage address — the allowlist guarantees a phone
    // sender, but a malformed IP string must not poison the video target.
    if (inet_pton(AF_INET, phoneIp, &m_serverAddr.sin_addr) != 1) {
        DriverLog("SwitchDataTarget: inet_pton rejected '%s', target unchanged", phoneIp);
        return;
    }
    m_hasPhoneTarget.store(true, std::memory_order_relaxed);

    DebugLog("Data target switched to %s:%d (phone copy enabled alongside local preview)", phoneIp, wire::kDataPort);
}