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

    while (m_discoveryRunning) {
        int bytesReceived = recvfrom(m_discoverySocket, buffer, sizeof(buffer) - 1, 0,
                                     (sockaddr*)&senderAddr, &senderAddrLen);

        if (!m_discoveryRunning) {
            break;
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

            // Switch data target to the phone's IP
            SwitchDataTarget(senderIpStr);

            // Send acknowledgment back to the phone (wire constant "ACK")
            sockaddr_in responseAddr;
            responseAddr.sin_family = AF_INET;
            responseAddr.sin_port = senderAddr.sin_port;
            responseAddr.sin_addr.s_addr = senderAddr.sin_addr.s_addr;
            sendto(m_discoverySocket, wire::kDiscoveryAck, (int)wire::kDiscoveryAckLen, 0,
                   (sockaddr*)&responseAddr, sizeof(responseAddr));

            DriverLog("Discovery ACK sent to %s", senderIpStr);
        }
    }

    DriverLog("Discovery thread exiting");
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

    DriverLog("Data target switched to %s:%d", phoneIp, wire::kDataPort);
}