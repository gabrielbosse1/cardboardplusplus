#include "HmdDriver.h"
#include "DriverLog.h"
#include "CardboardWire.h"
#include <cstdio>
#include <cstring>

using namespace vr;

// ---------------------------------------------------------------------------
// Phone discovery: broadcast UDP exchange that learns the phone's IP and
// reports its hardware decoder cap.
//
// The phone broadcasts on port 42070; every non-cap packet makes the driver
// switch the video stream (UdpTransport.cpp) to the sender's IP and ACK it.
// A "CARDBOARD_CAP <w> <h>" packet only reconfigures the encoder via
// ApplyHardwareCap() and is NOT acknowledged (see the wire contract in
// CardboardWire.h).
// ---------------------------------------------------------------------------

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

    char buffer[256];
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
                sockaddr_in responseAddr;
                responseAddr.sin_family = AF_INET;
                responseAddr.sin_port = senderAddr.sin_port;
                responseAddr.sin_addr.s_addr = senderAddr.sin_addr.s_addr;
                sendto(m_discoverySocket, wire::kBridgeAck, (int)wire::kBridgeAckLen, 0,
                       (sockaddr*)&responseAddr, sizeof(responseAddr));
                SendBridgeStats(responseAddr);
                DriverLog("BRIDGE_HELLO from %s:%d acked with BRIDGE_ACK + BRIDGE_STATS (data target untouched)", senderIpStr, ntohs(senderAddr.sin_port));
                continue;
            }

            if (strncmp(buffer, wire::kBridgePreview, wire::kBridgePreviewLen) == 0) {
                // Bridge toggles the localhost preview stream: "BRIDGE_PREVIEW 1"
                // keeps 127.0.0.1:42069 flowing (bridge UI / ffplay), "BRIDGE_PREVIEW 0"
                // cuts it. Only the preview target is affected, never the phone's.
                bool enabled = (strstr(buffer, "1") != nullptr);
                m_previewEnabled.store(enabled, std::memory_order_relaxed);
                DriverLog("BRIDGE_PREVIEW from %s:%d -> local preview %s", senderIpStr, ntohs(senderAddr.sin_port), enabled ? "ON" : "OFF");
                continue;
            }

            if (strncmp(buffer, wire::kBridgeCfg, wire::kBridgeCfgLen) == 0) {
                // The bridge pushes stream settings (BRIDGE_CFG <fps> <bitrate> <codec>)
                // on the same socket. It is control-plane traffic, not a phone — ignore.
                DriverLog("BRIDGE_CFG from %s:%d ignored (control-plane)", senderIpStr, ntohs(senderAddr.sin_port));
                continue;
            }

            // Switch data target to the phone's IP
            SwitchDataTarget(senderIpStr);
            m_lastPhonePacketMs.store(GetTickCount64(), std::memory_order_relaxed);

            // Send acknowledgment back to the phone (wire constant "ACK")
            sockaddr_in responseAddr;
            responseAddr.sin_family = AF_INET;
            responseAddr.sin_port = senderAddr.sin_port;
            responseAddr.sin_addr.s_addr = senderAddr.sin_addr.s_addr;
            sendto(m_discoverySocket, wire::kDiscoveryAck, (int)wire::kDiscoveryAckLen, 0,
                   (sockaddr*)&responseAddr, sizeof(responseAddr));

            DriverLog("Discovery ACK sent to %s", senderIpStr);
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
static void AppendPaddedNumber(char* buf, int* pos, int capacity, unsigned long long value)
{
    char tmp[24];
    sprintf_s(tmp, sizeof(tmp), "%llu", value);
    // Copy without a leading colon so the payload ends up "fps=<n>" etc.
    size_t need = strlen(tmp);
    if (*pos + (int)need < capacity) {
        memcpy(buf + *pos, tmp, need);
        *pos += (int)need;
    }
}

void HmdDriver::SendBridgeStats(const sockaddr_in& addr)
{
    char stats[160];
    // "BRIDGE_STATS fps=<fps> bitrate=<kbps> frames=<frames> drops=<drops>"
    int n = sprintf_s(stats, sizeof(stats), "%s fps=%d bitrate=", wire::kBridgeStats, m_encoderFps);
    if (n > 0) {
        AppendPaddedNumber(stats, &n, (int)sizeof(stats), (unsigned long long)(m_encoderBitrate / 1000));
        // Copy tag WITHOUT the literal's NUL terminator: " frames=" is 8 bytes,
        // " drops=" is 7. A stray NUL between '=' and the digits would make the
        // bridge parse the field as empty.
        static const char kFramesTag[] = " frames=";
        static const char kDropsTag[] = " drops=";
        if (n + (int)sizeof(kFramesTag) - 1 < (int)sizeof(stats)) {
            memcpy(stats + n, kFramesTag, sizeof(kFramesTag) - 1);
            n += (int)sizeof(kFramesTag) - 1;
        }
        AppendPaddedNumber(stats, &n, (int)sizeof(stats), m_udpFramesSent.load(std::memory_order_relaxed));
        if (n + (int)sizeof(kDropsTag) - 1 < (int)sizeof(stats)) {
            memcpy(stats + n, kDropsTag, sizeof(kDropsTag) - 1);
            n += (int)sizeof(kDropsTag) - 1;
        }
        AppendPaddedNumber(stats, &n, (int)sizeof(stats), m_udpDroppedFrames);
        sendto(m_discoverySocket, stats, n, 0, (sockaddr*)&addr, sizeof(addr));
    }
}

void HmdDriver::SwitchDataTarget(const char* phoneIp)
{
    std::lock_guard<std::mutex> lock(m_targetIpMutex);

    char currentIp[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &m_serverAddr.sin_addr, currentIp, sizeof(currentIp));

    if (strcmp(currentIp, phoneIp) == 0) {
        DriverLog("Data target already set to %s, skipping", phoneIp);
        return;
    }

    DriverLog("Switching data target from %s to %s", currentIp, phoneIp);

    m_serverAddr.sin_family = AF_INET;
    m_serverAddr.sin_port = htons(wire::kDataPort);
    inet_pton(AF_INET, phoneIp, &m_serverAddr.sin_addr);
    m_hasPhoneTarget.store(true, std::memory_order_relaxed);

    DriverLog("Data target switched to %s:%d (phone copy enabled alongside local preview)", phoneIp, wire::kDataPort);
}